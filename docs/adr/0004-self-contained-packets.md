# 0004 Self-contained packets

## Context

Steim2, as used in miniSEED, computes the first difference of a record
against the last sample of the previous record. On a lossy link the
previous packet may arrive later, or never.

## Decision

Every DATA packet decodes on its own. The first Steim2 difference is
stored as 0; the first sample comes from the frame's X0 word. Each packet
carries its own timestamp and sample period. The retransmitted bytes are
identical to the original, so a packet's meaning never depends on when it
arrives.

## Consequences

- The server can store packets in any order and reassemble by sequence
  number alone.
- One extra difference per packet is wasted (four bits at best).
- On export to miniSEED the samples are re-encoded; the wire format is not
  the archive format, and does not try to be.
