/*
 * main_posix.c — the station as a Linux process.
 *
 * Reads samples from a file at a configurable rate (optionally faster than
 * real time), feeds them to the protocol core, pumps the socket, and prints
 * statistics as JSON on exit. Everything the MCU application will do, minus
 * the interrupts.
 *
 * Storage is static: the ring buffer lives in a fixed array sized for the
 * largest supported --slots, so this binary allocates nothing after start,
 * exactly like the firmware build.
 */

#include "hal/hal_adc.h"
#include "hal/hal_log.h"
#include "hal/hal_net.h"
#include "hal/hal_time.h"
#include "station.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_SLOTS 4096 /* 1 MiB of retention at 256 bytes a slot */

/* Packets that may have been built between the last save of the sequence
 * counter and a crash. The resumed counter skips this many so no number is
 * ever reused; the skipped range is reported to the server as evicted. */
#define SEQ_RESUME_MARGIN 64

static uint8_t   slot_storage[MAX_SLOTS * PKT_MAX_LEN];
static uint16_t  slot_lengths[MAX_SLOTS];
static station_t station;

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

typedef struct {
    station_cfg_t cfg;
    const char   *server_host;
    uint16_t      server_port;
    const char   *signal_path;
    int           loop;
    unsigned      sample_rate_hz;
    size_t        slots;
    double        speedup;
    unsigned      linger_s;
    unsigned      stats_every_s;
    int           log_level;
    const char   *seq_file;
    uint64_t      skip_samples;
} options_t;

static void usage(void)
{
    fputs(
        "usage: station --signal FILE [options]\n"
        "\n"
        "  --id N              station id (1)\n"
        "  --stream N          stream id (1)\n"
        "  --server HOST:PORT  ingest server (127.0.0.1:5000)\n"
        "  --signal FILE       raw little-endian int32 samples (required)\n"
        "  --loop              wrap the signal file instead of stopping\n"
        "  --rate HZ           sample rate (100)\n"
        "  --spp N             samples per packet (200)\n"
        "  --encoding E        raw | steim2 (steim2)\n"
        "  --heartbeat MS      heartbeat interval (15000)\n"
        "  --rate-limit BPS    bytes per second, 0 = unlimited (0)\n"
        "  --burst BYTES       rate limiter burst (1024)\n"
        "  --slots N           retention slots, max 4096 (1024)\n"
        "  --speedup X         run signal time X times faster (1.0)\n"
        "  --linger S          keep serving backfill S seconds after the\n"
        "                      signal ends (30)\n"
        "  --stats-every S     print stats every S seconds, 0 = off (0)\n"
        "  --log-level N       0 error .. 3 debug (2)\n"
        "  --seq-file FILE     persist the sequence counter here and resume\n"
        "                      from it after a restart\n"
        "  --skip-samples N    start N samples into the signal file\n",
        stderr);
}

static int parse_server(const char *s, const char **host, uint16_t *port)
{
    static char hostbuf[256];
    const char *colon = strrchr(s, ':');
    if (colon == NULL || (size_t)(colon - s) >= sizeof hostbuf)
        return -1;
    memcpy(hostbuf, s, (size_t)(colon - s));
    hostbuf[colon - s] = '\0';
    const long p = strtol(colon + 1, NULL, 10);
    if (p <= 0 || p > 65535)
        return -1;
    *host = hostbuf;
    *port = (uint16_t)p;
    return 0;
}

static int parse_args(int argc, char **argv, options_t *o)
{
    memset(o, 0, sizeof *o);
    o->cfg.station_id            = 1;
    o->cfg.stream_id             = 1;
    o->cfg.samples_per_packet    = 200;
    o->cfg.encoding              = PKT_ENC_STEIM2;
    o->cfg.heartbeat_interval_ms = 15000;
    o->cfg.rate_burst_bytes      = 1024;
    o->server_host               = "127.0.0.1";
    o->server_port               = 5000;
    o->sample_rate_hz            = 100;
    o->slots                     = 1024;
    o->speedup                   = 1.0;
    o->linger_s                  = 30;
    o->log_level                 = HAL_LOG_INFO;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define NEED_VALUE() do { if (v == NULL) { usage(); return -1; } i++; } while (0)
        if (strcmp(a, "--loop") == 0) {
            o->loop = 1;
        } else if (strcmp(a, "--id") == 0) {
            NEED_VALUE();
            o->cfg.station_id = (uint32_t)strtoul(v, NULL, 0);
        } else if (strcmp(a, "--stream") == 0) {
            NEED_VALUE();
            o->cfg.stream_id = (uint16_t)strtoul(v, NULL, 0);
        } else if (strcmp(a, "--server") == 0) {
            NEED_VALUE();
            if (parse_server(v, &o->server_host, &o->server_port) != 0)
                return -1;
        } else if (strcmp(a, "--signal") == 0) {
            NEED_VALUE();
            o->signal_path = v;
        } else if (strcmp(a, "--rate") == 0) {
            NEED_VALUE();
            o->sample_rate_hz = (unsigned)strtoul(v, NULL, 10);
        } else if (strcmp(a, "--spp") == 0) {
            NEED_VALUE();
            o->cfg.samples_per_packet = (uint16_t)strtoul(v, NULL, 10);
        } else if (strcmp(a, "--encoding") == 0) {
            NEED_VALUE();
            if (strcmp(v, "raw") == 0)
                o->cfg.encoding = PKT_ENC_RAW;
            else if (strcmp(v, "steim2") == 0)
                o->cfg.encoding = PKT_ENC_STEIM2;
            else
                return -1;
        } else if (strcmp(a, "--heartbeat") == 0) {
            NEED_VALUE();
            o->cfg.heartbeat_interval_ms = (uint32_t)strtoul(v, NULL, 10);
        } else if (strcmp(a, "--rate-limit") == 0) {
            NEED_VALUE();
            o->cfg.rate_limit_bps = (uint32_t)strtoul(v, NULL, 10);
        } else if (strcmp(a, "--burst") == 0) {
            NEED_VALUE();
            o->cfg.rate_burst_bytes = (uint32_t)strtoul(v, NULL, 10);
        } else if (strcmp(a, "--slots") == 0) {
            NEED_VALUE();
            o->slots = (size_t)strtoul(v, NULL, 10);
        } else if (strcmp(a, "--speedup") == 0) {
            NEED_VALUE();
            o->speedup = strtod(v, NULL);
        } else if (strcmp(a, "--linger") == 0) {
            NEED_VALUE();
            o->linger_s = (unsigned)strtoul(v, NULL, 10);
        } else if (strcmp(a, "--stats-every") == 0) {
            NEED_VALUE();
            o->stats_every_s = (unsigned)strtoul(v, NULL, 10);
        } else if (strcmp(a, "--log-level") == 0) {
            NEED_VALUE();
            o->log_level = (int)strtol(v, NULL, 10);
        } else if (strcmp(a, "--seq-file") == 0) {
            NEED_VALUE();
            o->seq_file = v;
        } else if (strcmp(a, "--skip-samples") == 0) {
            NEED_VALUE();
            o->skip_samples = strtoull(v, NULL, 10);
        } else {
            usage();
            return -1;
        }
#undef NEED_VALUE
    }

    if (o->signal_path == NULL || o->sample_rate_hz == 0 || o->speedup <= 0 ||
        o->slots == 0 || o->slots > MAX_SLOTS) {
        usage();
        return -1;
    }
    o->cfg.sample_period_us = 1000000u / o->sample_rate_hz;
    return 0;
}

/* The persisted counter: a decimal number in a file, replaced atomically so
 * a crash mid-write leaves the old value, never a torn one. On the MCU this
 * becomes a flash journal entry; the contract is the same. */
static uint64_t read_seq_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return 0;
    unsigned long long v = 0;
    if (fscanf(f, "%llu", &v) != 1)
        v = 0;
    fclose(f);
    return v;
}

static void write_seq_file(const char *path, uint64_t seq)
{
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return;
    FILE *f = fopen(tmp, "w");
    if (f == NULL)
        return;
    fprintf(f, "%llu\n", (unsigned long long)seq);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    rename(tmp, path);
}

static void print_stats(const station_t *st, const char *phase, uint64_t now_ns)
{
    const station_stats_t *s = &st->stats;
    printf("{\"phase\":\"%s\",\"uptime_s\":%llu,\"samples_in\":%llu,"
           "\"packets_built\":%llu,\"live_sent\":%llu,\"live_deferred\":%llu,"
           "\"retransmits\":%llu,\"heartbeats\":%llu,\"bytes_sent\":%llu,"
           "\"gaps_received\":%llu,\"gap_seqs_evicted\":%llu,"
           "\"gap_seqs_future\":%llu,\"rx_bad\":%llu,\"send_errors\":%llu,"
           "\"retained\":%zu,\"next_seq\":%llu,\"oldest_seq\":%llu}\n",
           phase, (unsigned long long)((now_ns - st->start_ns) / 1000000000ull),
           (unsigned long long)s->samples_in, (unsigned long long)s->packets_built,
           (unsigned long long)s->live_sent, (unsigned long long)s->live_deferred,
           (unsigned long long)s->retransmits, (unsigned long long)s->heartbeats,
           (unsigned long long)s->bytes_sent, (unsigned long long)s->gaps_received,
           (unsigned long long)s->gap_seqs_evicted, (unsigned long long)s->gap_seqs_future,
           (unsigned long long)s->rx_bad, (unsigned long long)s->send_errors,
           rb_count(&st->rb), (unsigned long long)rb_next_seq(&st->rb),
           (unsigned long long)rb_oldest_seq(&st->rb));
    fflush(stdout);
}

int main(int argc, char **argv)
{
    options_t o;
    if (parse_args(argc, argv, &o) != 0)
        return 2;

    hal_log_set_level(o.log_level);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (hal_adc_init(o.signal_path, o.loop) != 0) {
        hal_log(HAL_LOG_ERROR, "cannot open signal file %s", o.signal_path);
        return 1;
    }
    if (hal_net_init(o.server_host, o.server_port) != 0) {
        hal_log(HAL_LOG_ERROR, "cannot resolve %s:%u", o.server_host, o.server_port);
        return 1;
    }

    if (o.seq_file != NULL) {
        const uint64_t saved = read_seq_file(o.seq_file);
        if (saved > 0) {
            o.cfg.start_seq = saved + SEQ_RESUME_MARGIN;
            hal_log(HAL_LOG_INFO, "resuming sequence numbers at %llu (saved %llu + margin %d)",
                    (unsigned long long)o.cfg.start_seq, (unsigned long long)saved,
                    SEQ_RESUME_MARGIN);
        }
    }

    const uint64_t start_mono = hal_time_mono_ns();
    if (station_init(&station, &o.cfg, slot_storage, slot_lengths, o.slots, start_mono) != 0) {
        hal_log(HAL_LOG_ERROR, "bad station configuration");
        return 2;
    }
    station_set_clock_quality(&station, hal_time_clock_quality());

    hal_log(HAL_LOG_INFO, "station %u -> %s:%u, %u Hz, %s, %zu slots, x%.1f",
            o.cfg.station_id, o.server_host, o.server_port, o.sample_rate_hz,
            o.cfg.encoding == PKT_ENC_RAW ? "raw" : "steim2", o.slots, o.speedup);

    const uint64_t period_ns  = (uint64_t)o.cfg.sample_period_us * 1000ull;
    const uint64_t wall_start = hal_time_wall_ns();
    uint64_t       sample_idx = 0;
    uint64_t       saved_seq  = rb_next_seq(&station.rb);
    int            signal_done = 0;
    uint64_t       signal_end_mono = 0;
    uint64_t       last_stats = start_mono;
    int32_t        chunk[256];
    uint8_t        rx[PKT_MAX_LEN];

    for (uint64_t skipped = 0; skipped < o.skip_samples;) {
        size_t want = (size_t)(o.skip_samples - skipped);
        if (want > sizeof chunk / sizeof chunk[0])
            want = sizeof chunk / sizeof chunk[0];
        const int n = hal_adc_read(chunk, want);
        if (n <= 0)
            break;
        skipped += (uint64_t)n;
    }

    while (!stop_requested) {
        const uint64_t now = hal_time_mono_ns();

        if (!signal_done) {
            /* How many samples should exist by now, in signal time. */
            const double   elapsed = (double)(now - start_mono) * o.speedup;
            const uint64_t due     = (uint64_t)(elapsed / (double)period_ns);
            while (sample_idx < due) {
                size_t want = (size_t)(due - sample_idx);
                if (want > sizeof chunk / sizeof chunk[0])
                    want = sizeof chunk / sizeof chunk[0];
                const int n = hal_adc_read(chunk, want);
                if (n < 0) {
                    signal_done     = 1;
                    signal_end_mono = now;
                    station_flush(&station, now);
                    hal_log(HAL_LOG_INFO, "signal ended after %llu samples; lingering %us",
                            (unsigned long long)sample_idx, o.linger_s);
                    break;
                }
                if (n == 0)
                    break;
                const uint64_t t_first = wall_start + sample_idx * period_ns;
                size_t fed = 0;
                while (fed < (size_t)n)
                    fed += station_feed(&station, chunk + fed, (size_t)n - fed, t_first + fed * period_ns, now);
                sample_idx += (uint64_t)n;
            }
        } else if (now - signal_end_mono >= (uint64_t)o.linger_s * 1000000000ull) {
            break;
        }

        const int r = hal_net_recv(rx, sizeof rx, 5);
        if (r > 0)
            station_on_rx(&station, rx, (size_t)r, now);

        station_tick(&station, now);

        if (o.seq_file != NULL && rb_next_seq(&station.rb) != saved_seq) {
            saved_seq = rb_next_seq(&station.rb);
            write_seq_file(o.seq_file, saved_seq);
        }

        if (o.stats_every_s > 0 && now - last_stats >= (uint64_t)o.stats_every_s * 1000000000ull) {
            print_stats(&station, "running", now);
            last_stats = now;
        }
    }

    const uint64_t now = hal_time_mono_ns();
    station_flush(&station, now);
    station_tick(&station, now);
    if (o.seq_file != NULL)
        write_seq_file(o.seq_file, rb_next_seq(&station.rb));
    print_stats(&station, stop_requested ? "stopped" : "finished", now);

    hal_net_close();
    hal_adc_close();
    return 0;
}
