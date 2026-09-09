# 0006 Virtual hardware first

## Context

No development board was available when the project started, and a
project that needs a board on a desk to test cannot test in CI.

## Decision

Every milestone is proven on virtual hardware before real hardware: the
host build with a file-replay ADC and `tools/netsim.py` for the link now;
Zephyr `native_sim` and Renode with a modelled STM32 next. Real boards are
a port at the end, not a prerequisite at the start.

## Consequences

- CI runs the full pipeline through hostile network profiles on every
  push; no lab is needed.
- Things that only real hardware shows (analogue noise, timing of a real
  ADC, a real modem's behaviour) are deferred, and the design keeps them
  behind the HAL so they arrive as implementation, not redesign.
- GDB against Renode stands in for JTAG; Renode's peripheral trace stands
  in for a logic analyzer, until there is one.
