# Results and how they were measured

Everything here was produced by scripts in this repository. Re-run them and you should see the same
kind of numbers, not the same digits: the QNX timings depend on the load of the machine under QEMU.

## Setup

| | |
|---|---|
| Operating system | QNX SDP 8.0 (`QNX 8.0.0 2026/06/05 QEMU_virt aarch64le`) |
| Emulator | QEMU 11.1.3, machine `virt-8.2`, CPU `cortex-a57`, 2 vCPUs, **TCG (software emulation)** |
| Host | Apple M4, 16 GB, macOS |
| Control loop | 250 Hz (4 ms), AI at 50 Hz, proposal freshness 100 ms, hardware watchdog 200 ms |
| Patient | Healthy single-compartment lung: R = 10 cmH₂O/(L/s), C = 0.05 L/cmH₂O. **Simulated** |

The defaults in `core/` are 1 kHz control, 100 Hz AI, 25 ms freshness and a 50 ms watchdog. QEMU
under software emulation cannot keep those (see below), so the image ships
[`qnx/image/tether.flags`](../qnx/image/tether.flags).

### Why 250 Hz

A timer-pulse loop with nothing else running, on this emulator, delivered 437 pulses per second when
asked for 1,000 (1.7 s behind after 3 s). At 2 ms it delivered 492 of 500, at 4 ms 248 of 250, at
10 ms exactly 100. `ClockPeriod()` reported `Not supported` on this platform, so the 1 ms kernel
tick could not be changed. 250 Hz is the highest rate that stays on time.

QEMU's Apple-silicon accelerator (HVF) cannot be used: QEMU stops with `Assertion failed: (isv)`
while the QNX kernel starts.

## Pass/fail checks on the live system

`tools/qemu/selftest.sh` — 29 checks, all pass:

```
PASS  uid 2002 cannot open /dev/tether/actuator
PASS  hardware counted the refused open
PASS  uid 2002 refused 5 of 5 operator actions (guard, fault, reset, model swap, lung)
PASS  none of them changed anything: the AI is still in control
PASS  spike:  arbiter switched to SAFE (reason range)              and the AI was readmitted
PASS  nan:    arbiter switched to SAFE (reason non-finite)         and the AI was readmitted
PASS  stuck:  arbiter switched to SAFE (reason expiration-pressure) and the AI was readmitted
PASS  drift:  arbiter switched to SAFE (reason predicted-overdistension) and the AI was readmitted
PASS  bypass: 298 opens refused by the OS in 6 s
PASS  hog:    control loop kept running (2028 ticks in 8 s, 250 Hz)
PASS  crash:  supervisor brought the AI back; AI back in control
PASS  hang:   supervisor brought the AI back; AI back in control
PASS  guard off + spike: 1249 ticks over the envelope (shows what Tether prevents)
PASS  model swapped to vent-unvalidated
PASS  unvalidated model: no violation in 20 s
PASS  swapped back to vent-v1, AI readmitted
PASS  ards:   AI in control, tidal volume 0.4165 L, estimated R=11.91 C=0.0250   (true 12 / 0.025)
PASS  asthma: AI in control, tidal volume 0.3820 L, estimated R=26.19 C=0.0435   (true 25 / 0.045)
PASS  healthy: AI in control, tidal volume 0.4136 L, estimated R=9.95 C=0.0500   (true 10 / 0.05)
PASS  no envelope violation outside the deliberate guard-off test
PASS  tether-arbiter killed: supervisor restarted everything, AI back in control
PASS  tether-hw killed:      supervisor restarted everything, AI back in control
PASS  tether-bridge killed:  restarted on its own
```

(Some lines above merge two checks, for example a fault and the AI's readmission after it.)

The guard-off test is the control experiment: with the arbiter passing the AI's output straight to
the blower, the same `spike` fault breaks the limits. Violations are ticks on which the *measured*
airway pressure exceeded the limit by more than 0.5 cmH₂O, or the delivered volume exceeded its
limit by more than 10 mL, whoever was driving.

## Wake-up latency of the control tick

`tools/qemu/latency-test.sh 25 none hog bypass slow none`. `tether-ctl reset` starts a new window
before each stress so the stresses do not average each other away. Time from the ideal expiry of the
timer to the control thread running, µs.

| window | n | p50 | p99 | p99.9 | max | skipped ticks (running total) |
|---|---|---|---|---|---|---|
| idle | 6249 | 1250 | 2010 | 3490 | 8234 | 0 |
| AI saturating both CPUs (`hog`) | 6256 | 1140 | 1910 | 11600 | 30994 | 0 |
| AI spamming refused opens (`bypass`) | 6258 | 1230 | 2000 | 2400 | 4456 | 0 |
| AI 40 ms per inference (`slow`) | 6236 | 1280 | 2060 | 7970 | 54042 | 13 |
| idle again | 6247 | 1250 | 5310 | 25600 | 35723 | 13 |

How to read it:

* **The stresses do not move the median or the 99th percentile.** During `hog`, `top` showed the
  machine 99.1% busy in user mode (4.3% before), with the two AI threads at about 48% each and the
  priority-60 arbiter thread still running.
* **The maxima are the emulator and the host.** The second idle window is worse than the stressed
  ones, and the 13 skipped ticks (a running total that `reset` does not clear) appeared during `slow`,
  a fault that cannot touch the control loop.
  The Mac was running other work (load average about 3.7). On a quieter minute an earlier run gave an
  idle p99 of 1.7 ms and a max of 2 ms.
* **A p50 near 1 ms is the 1 ms kernel tick**, not the cost of the code: a timer set for 4 ms fires
  at the next tick boundary. The loop's own work is the `exec` figure, about 0.7 ms at p99.

These figures do not describe real hardware. They show that scheduling priority, not good behaviour
in the AI, keeps the loop running.

## Fault campaign on the host

`build/host/tether-sim --model models/vent-v1.tmlp --campaign`: the worst case over 8 fault-onset
times inside one breath, healthy lung. Pressure in cmH₂O, volume in litres, limits 35 and 0.56.
"Time over limits" adds the seconds over the pressure limit to the seconds over the volume limit.

| fault | AI alone: peak pressure | AI alone: peak volume | time over limits | with Tether: peak pressure | peak volume | time over limits |
|---|---|---|---|---|---|---|
| none | 29.8 | 0.462 | 0 s | 15.0 | 0.417 | 0 s |
| `nan` | 29.8 | 0.878 | 0.9 s | 15.0 | 0.417 | 0 s |
| `spike` | 70.0 | 3.250 | 17.4 s | 15.0 | 0.417 | 0 s |
| `stuck` | 29.8 | 0.880 | 1.1 s | 15.1 | 0.427 | 0 s |
| `drift` | 29.8 | 0.550 | 0 s | 16.1 | 0.454 | 0 s |

A hostile AI that returns arbitrary values for 2.4 million ticks never took the system outside the
envelope: peak 27.9 cmH₂O and 0.466 L. The tests run on every build (`ctest`, 775 checks).

A model biased to overshoot by 12 cmH₂O, run without Tether on the host, reaches 44.4 cmH₂O and
1.88 L and spends 39.8 s over the volume limit. Behind Tether it peaks at 14.0 cmH₂O and 0.38 L.

## What these results do not show

* Nothing about a real ventilator, a real patient, or real hardware timing.
* That the AI is accurate. The monitor guards the safety limits, not the AI's accuracy. While the
  recovery controller is in charge it delivers 0.38 L on a healthy lung against a 0.42 L target, about
  10% low.
* Detection of a drift that stays inside the envelope until a breath ends: the lookahead catches it
  after a few breaths (3.6 s in the campaign), not instantly.
