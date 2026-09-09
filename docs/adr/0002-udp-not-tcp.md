# 0002 UDP, not TCP

## Context

TCP would give ordering and retransmission for free. On the links in
question it also gives head-of-line blocking, connection state that dies
with every modem reset, exponential backoff that turns a 30-second blackout
into minutes of silence, and 40-byte headers on 256-byte packets.

## Decision

UDP datagrams, one message each, with the protocol's own sequence numbers,
retention buffer and gap requests. Loss is handled by the application,
where the application can decide that fresh data matters more than old.

## Consequences

- No connection to re-establish after a blackout; the first datagram
  through is data.
- The station chooses what to resend and when; the rate limiter meters
  everything through one bucket.
- The protocol must be idempotent and tolerate duplication and reordering.
  It is: the server keys packets by (station, seq).
- A 36-byte header at the application layer instead of 40+ at TCP.
