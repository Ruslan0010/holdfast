"""End to end through a hostile network.

For each link profile: generate a signal, start the server, put the network
simulator between them, run the station faster than real time, wait until
the server reports nothing missing, then reassemble from the database and
compare with the source byte for byte.

Results are appended to build/integration_results.json; the README table is
built from that file.

    make integration            # all profiles
    HOLDFAST_PROFILES=clean,dying-modem make integration
"""

from __future__ import annotations

import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STATION = os.path.join(ROOT, "build", "station")
RESULTS = os.path.join(ROOT, "build", "integration_results.json")
PY = sys.executable
ENV = dict(os.environ, PYTHONPATH=os.path.join(ROOT, "server"), PYTHONUNBUFFERED="1")

SIGNAL_SECONDS = 240
RATE = 100
SPEEDUP = 20
TIME_SCALE = 0.03  # blackout schedule: dying-modem blacks out 1.8 s every 9 s
COMPLETE_TIMEOUT = 120


def free_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Pipeline:
    def __init__(self, name: str, profile: str, encoding: str = "steim2",
                 rate_limit: int = 0, slots: int = 1024, signal_seconds: int = SIGNAL_SECONDS):
        self.name = name
        self.profile = profile
        self.encoding = encoding
        self.rate_limit = rate_limit
        self.slots = slots
        self.signal_seconds = signal_seconds
        self.tmp = tempfile.mkdtemp(prefix=f"holdfast-{name}-")
        self.procs: list[subprocess.Popen] = []
        self.logs: dict[str, str] = {}
        self.handles: list = []
        self.skip_samples = 0

    def path(self, *parts: str) -> str:
        return os.path.join(self.tmp, *parts)

    def spawn(self, name: str, cmd: list[str], stdout=None) -> subprocess.Popen:
        log = self.path(name + ".log")
        self.logs[name] = log
        err = open(log, "a")
        out = open(stdout, "a") if stdout else subprocess.DEVNULL
        self.handles += [err] + ([out] if stdout else [])
        proc = subprocess.Popen(cmd, cwd=ROOT, env=ENV, stdout=out, stderr=err)
        self.procs.append(proc)
        return proc

    def start(self, station: bool = True) -> None:
        subprocess.run([PY, "tools/gen_signal.py", "--seconds", str(self.signal_seconds),
                        "--rate", str(RATE), "--seed", "1", "--out", self.path("signal.i32")],
                       cwd=ROOT, check=True, capture_output=True)
        self.server_port = free_udp_port()
        self.sim_port = free_udp_port()
        self.spawn("server", [
            PY, "-m", "holdfast.ingest", "--bind", "127.0.0.1", "--port", str(self.server_port),
            "--db", self.path("holdfast.db"), "--status", self.path("status.json"),
            "--gap-interval", "1", "--gap-max-interval", "8", "--tick", "0.1",
            "--link-sample-every", "2"])
        self.spawn("netsim", [
            PY, "tools/netsim.py", "--listen", f"127.0.0.1:{self.sim_port}",
            "--upstream", f"127.0.0.1:{self.server_port}", "--profile", self.profile,
            "--time-scale", str(TIME_SCALE), "--seed", "1",
            "--stats-file", self.path("netsim.json")])
        time.sleep(0.5)
        self.t_start = time.monotonic()
        if station:
            self.start_station()

    def start_station(self, skip_samples: int = 0, seq_file: str | None = None) -> None:
        self.skip_samples = skip_samples
        cmd = [STATION, "--id", "1", "--server", f"127.0.0.1:{self.sim_port}",
               "--signal", self.path("signal.i32"), "--rate", str(RATE), "--spp", "300",
               "--encoding", self.encoding, "--heartbeat", "1000", "--speedup", str(SPEEDUP),
               "--linger", "600", "--slots", str(self.slots), "--stats-every", "1",
               "--log-level", "2"]
        if self.rate_limit:
            cmd += ["--rate-limit", str(self.rate_limit), "--burst", "2048"]
        if seq_file:
            cmd += ["--seq-file", seq_file]
        if skip_samples:
            cmd += ["--skip-samples", str(skip_samples)]
        if os.path.exists(self.path("station.jsonl")):
            os.remove(self.path("station.jsonl"))
        self.station = self.spawn("station", cmd, stdout=self.path("station.jsonl"))

    def stop_station(self) -> dict:
        """SIGTERM the station and return its final statistics."""
        self.station.send_signal(signal.SIGTERM)
        self.station.wait(timeout=10)
        stats = self.station_stats()
        assert stats and stats["phase"] == "stopped", stats
        return stats

    def station_stats(self) -> dict | None:
        try:
            lines = open(self.path("station.jsonl")).read().strip().splitlines()
        except FileNotFoundError:
            return None
        return json.loads(lines[-1]) if lines else None

    def status(self) -> dict | None:
        try:
            return json.load(open(self.path("status.json")))["stations"].get("1")
        except (FileNotFoundError, ValueError, KeyError):
            return None

    def wait_complete(self, timeout: float = COMPLETE_TIMEOUT) -> tuple[dict, dict]:
        """Wait until the station has sent its last packet and the server has
        everything it can still get."""
        deadline = time.monotonic() + timeout
        total_samples = self.signal_seconds * RATE - self.skip_samples
        while time.monotonic() < deadline:
            if self.station.poll() is not None:
                raise AssertionError("station exited early; see " + self.logs["station"])
            st = self.station_stats()
            sv = self.status()
            if st and sv and st["samples_in"] == total_samples and st["retained"] > 0:
                # The station flushes its last packet in the same loop pass
                # that feeds the last sample, so samples_in == total means
                # every packet has been built and next_seq is final.
                if sv["complete"] and sv["next_seq"] == st["next_seq"] and not sv["missing"]:
                    return st, sv
            time.sleep(0.5)
        raise AssertionError(
            f"not complete after {timeout}s: station={self.station_stats()} server={self.status()}")

    def stop(self) -> None:
        for proc in reversed(self.procs):
            if proc.poll() is None:
                proc.send_signal(signal.SIGTERM)
        for proc in reversed(self.procs):
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        for h in self.handles:
            h.close()
        self.wall_seconds = time.monotonic() - self.t_start

    def verify(self) -> dict:
        r = subprocess.run([PY, "-m", "holdfast.export", "--db", self.path("holdfast.db"),
                            "--station", "1", "--out", self.path("got.i32"),
                            "--verify", self.path("signal.i32")],
                           cwd=ROOT, env=ENV, capture_output=True, text=True)
        summary = json.loads(r.stdout)
        summary["exit"] = r.returncode
        return summary

    def netsim_stats(self) -> dict:
        try:
            return json.load(open(self.path("netsim.json")))
        except FileNotFoundError:
            return {}

    def tail(self, name: str, n: int = 15) -> str:
        try:
            return "".join(open(self.logs[name]).readlines()[-n:])
        except (KeyError, FileNotFoundError):
            return ""


def record(result: dict) -> None:
    os.makedirs(os.path.dirname(RESULTS), exist_ok=True)
    try:
        results = json.load(open(RESULTS))
    except (FileNotFoundError, ValueError):
        results = []
    results = [r for r in results if r["name"] != result["name"]] + [result]
    with open(RESULTS, "w") as f:
        json.dump(results, f, indent=1)


def selected(name: str) -> bool:
    want = os.environ.get("HOLDFAST_PROFILES")
    return not want or name in want.split(",")


@unittest.skipUnless(os.path.exists(STATION), "build/station missing: run `make station`")
class TestPipeline(unittest.TestCase):
    def run_profile(self, name: str, profile: str, **kw) -> tuple[dict, dict, dict, dict]:
        if not selected(name):
            self.skipTest("not in HOLDFAST_PROFILES")
        pipe = Pipeline(name, profile, **kw)
        try:
            pipe.start()
            try:
                st, sv = pipe.wait_complete()
            finally:
                pipe.stop()
            summary = pipe.verify()
            net = pipe.netsim_stats()
        except Exception:
            print("\n--- station ---\n" + pipe.tail("station") +
                  "\n--- server ---\n" + pipe.tail("server") +
                  "\n--- netsim ---\n" + pipe.tail("netsim"), file=sys.stderr)
            raise

        up = net.get("up", {})
        result = {
            "name": name,
            "profile": profile,
            "encoding": kw.get("encoding", "steim2"),
            "rate_limit_bps": kw.get("rate_limit", 0),
            "slots": kw.get("slots", 1024),
            "signal_seconds": kw.get("signal_seconds", SIGNAL_SECONDS),
            "source_samples": summary["verify"]["source_samples"],
            "recovered_samples": summary["verify"]["recovered_samples"],
            "recovered_pct": summary["verify"]["recovered_pct"],
            "byte_for_byte": summary["verify"]["byte_for_byte"],
            "packets_built": st["packets_built"],
            "live_sent": st["live_sent"],
            "live_deferred": st["live_deferred"],
            "retransmits": st["retransmits"],
            "heartbeats": st["heartbeats"],
            "station_bytes_sent": st["bytes_sent"],
            "server_bytes_received": sv["bytes"],
            "bytes_per_sample_on_wire": round(st["bytes_sent"] / summary["verify"]["source_samples"], 3),
            "link_datagrams_in": up.get("in"),
            "link_dropped": up.get("dropped", 0) + up.get("blackout", 0),
            "link_blackouts": net.get("blackouts"),
            "late": sv["late"],
            "duplicates": sv["duplicates"],
            "gap_requests": sv["gap_requests"],
            "unrecoverable": sv["unrecoverable"],
            "problems": summary["problems"],
            "wall_seconds": round(pipe.wall_seconds, 1),
        }
        record(result)
        print(f"\n  {name}: {result['recovered_pct']}% recovered, "
              f"{result['retransmits']} retransmits, {result['link_dropped']} dropped, "
              f"{result['bytes_per_sample_on_wire']} B/sample, {result['wall_seconds']}s",
              file=sys.stderr)
        return st, sv, summary, result

    def assert_perfect(self, summary: dict, result: dict) -> None:
        self.assertEqual(summary["problems"], [])
        self.assertTrue(summary["verify"]["byte_for_byte"], summary)
        self.assertEqual(summary["verify"]["recovered_samples"], summary["verify"]["source_samples"])
        self.assertEqual(result["unrecoverable"], 0)
        self.assertEqual(summary["exit"], 0)

    def test_clean(self):
        st, sv, summary, result = self.run_profile("clean", "clean")
        self.assert_perfect(summary, result)
        self.assertEqual(result["retransmits"], 0)

    def test_clean_raw(self):
        """Same link without compression, to measure what Steim2 saves."""
        st, sv, summary, result = self.run_profile("clean-raw", "clean", encoding="raw")
        self.assert_perfect(summary, result)

    def test_starlink(self):
        st, sv, summary, result = self.run_profile("starlink", "starlink")
        self.assert_perfect(summary, result)
        self.assertGreater(result["link_dropped"], 0)
        self.assertGreater(result["retransmits"], 0)

    def test_4g_mountain(self):
        st, sv, summary, result = self.run_profile("4g-mountain", "4g-mountain")
        self.assert_perfect(summary, result)
        self.assertGreater(result["retransmits"], 0)

    def test_dying_modem(self):
        st, sv, summary, result = self.run_profile("dying-modem", "dying-modem")
        self.assert_perfect(summary, result)
        self.assertGreater(result["link_blackouts"], 0)
        self.assertGreater(result["retransmits"], 0)

    def test_dying_modem_rate_limited(self):
        """The link budget is below the live data rate at x20, so live
        packets are deferred and the ring buffer drains through backfill."""
        st, sv, summary, result = self.run_profile(
            "dying-modem-ratelimit", "dying-modem", rate_limit=6000)
        self.assert_perfect(summary, result)
        self.assertGreater(result["live_deferred"], 0)

    def test_tiny_ring_loses_data_honestly(self):
        """Retention of 8 packets and a blackout longer than that: some data
        is gone. The server must record it as unrecoverable, stop asking,
        and still deliver everything else intact."""
        st, sv, summary, result = self.run_profile(
            "dying-modem-tiny-ring", "dying-modem", slots=8, signal_seconds=120)
        self.assertGreater(result["unrecoverable"], 0)
        self.assertGreater(len(summary["problems"]), 0)
        holes = set()
        for p_ in summary["problems"]:
            if p_["kind"] == "missing_seq":
                holes.update(range(p_["from"], p_["to"] + 1))
        unrecoverable = set()
        for a, b in summary["unrecoverable_ranges"]:
            unrecoverable.update(range(a, b + 1))
        self.assertEqual(holes, unrecoverable)
        self.assertEqual(len(unrecoverable), result["unrecoverable"])
        self.assertLess(result["recovered_pct"], 100.0)

    def test_station_restart_resumes_sequence(self):
        """Kill the station mid-stream and start it again with the same
        persisted sequence counter. Numbers must not be reused: the restart
        shows up as one unrecoverable hole, and everything on either side
        of it is byte-identical to the source."""
        if not selected("restart"):
            self.skipTest("not in HOLDFAST_PROFILES")
        pipe = Pipeline("restart", "clean", signal_seconds=120)
        seq_file = pipe.path("seq")
        try:
            pipe.start(station=False)
            pipe.start_station(seq_file=seq_file)
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                st = pipe.station_stats()
                if st and st["packets_built"] >= 30:
                    break
                time.sleep(0.2)
            first = pipe.stop_station()
            skip = first["samples_in"]
            pipe.start_station(skip_samples=skip, seq_file=seq_file)
            try:
                second, sv = pipe.wait_complete()
            finally:
                pipe.stop()
            summary = pipe.verify()
        except Exception:
            print("\n--- station ---\n" + pipe.tail("station") +
                  "\n--- server ---\n" + pipe.tail("server"), file=sys.stderr)
            raise

        record({
            "name": "restart", "profile": "clean", "encoding": "steim2", "rate_limit_bps": 0,
            "slots": 1024, "signal_seconds": 120,
            "source_samples": summary["verify"]["source_samples"],
            "recovered_samples": summary["verify"]["recovered_samples"],
            "recovered_pct": summary["verify"]["recovered_pct"],
            "byte_for_byte": summary["verify"]["byte_for_byte"],
            "packets_built": first["packets_built"] + second["packets_built"],
            "live_sent": first["live_sent"] + second["live_sent"],
            "live_deferred": 0, "retransmits": first["retransmits"] + second["retransmits"],
            "heartbeats": first["heartbeats"] + second["heartbeats"],
            "station_bytes_sent": first["bytes_sent"] + second["bytes_sent"],
            "server_bytes_received": sv["bytes"],
            "bytes_per_sample_on_wire": round((first["bytes_sent"] + second["bytes_sent"]) / summary["verify"]["source_samples"], 3),
            "link_datagrams_in": None, "link_dropped": 0, "link_blackouts": 0,
            "late": sv["late"], "duplicates": sv["duplicates"], "gap_requests": sv["gap_requests"],
            "unrecoverable": sv["unrecoverable"], "problems": summary["problems"],
            "wall_seconds": round(pipe.wall_seconds, 1),
        })

        self.assertGreater(second["next_seq"], first["next_seq"] + 64)
        self.assertGreaterEqual(sv["unrecoverable"], 64)
        holes = [p_ for p_ in summary["problems"] if p_["kind"] == "missing_seq"]
        self.assertEqual(len(holes), 1, summary["problems"])

        src = open(pipe.path("signal.i32"), "rb").read()
        got = open(pipe.path("got.i32"), "rb").read()
        tail = src[skip * 4:]
        self.assertGreater(len(got), len(tail))
        k = len(got) - len(tail)
        self.assertEqual(got[:k], src[:k])   # before the restart
        self.assertEqual(got[k:], tail)      # after the restart


if __name__ == "__main__":
    unittest.main()
