# 0003 Fixed-size slots, no heap

## Context

The station runs unattended for years on a microcontroller. Heap
fragmentation on such a device is a slow-motion crash.

## Decision

The retention buffer is an array of 256-byte slots over caller-owned
memory, indexed by `seq % slot_count`. Nothing in the firmware core calls
`malloc`. The host binary follows the same rule: all storage is static.

## Consequences

- Retention is measured in packets, not bytes; a short packet wastes the
  rest of its slot. At one to three seconds of data per packet this is a
  small price.
- Every operation is O(1).
- Memory use is known at link time and does not change at run time.
- The slot size fixes the maximum datagram at 256 bytes, which also keeps
  every datagram under any link's MTU.
