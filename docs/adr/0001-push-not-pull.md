# 0001 Push, not pull

## Context

The station sits behind carrier-grade NAT on a cellular modem, or behind a
satellite terminal that only permits outbound traffic. There is no address
the server can connect to.

## Decision

The station opens every conversation. It pushes DATA and HEARTBEAT to a
fixed server address; the server only ever replies to the source address
of the last datagram it received, which is the NAT mapping the station
created. Heartbeats keep that mapping alive.

## Consequences

- Works on any link that allows outbound UDP, which is every link.
- The server cannot ask a silent station anything. If the station stops
  sending heartbeats, the server can only wait.
- The heartbeat interval must be shorter than the NAT's UDP idle timeout
  (commonly 30 s, sometimes 60 s); 15 s is the default.
