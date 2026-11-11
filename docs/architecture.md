# Architecture

Tether is a run-time assurance (Simplex) system. This page explains the parts, why they are
separate processes, and what each one is allowed to do.

## Roles

ASTM F3269 names four roles. Each has one home in the code.

| Role | What it does | Where |
|---|---|---|
| Input manager | Reads the sensor, integrates volume, keeps the breath clock, estimates the lung's compliance and resistance from the sensors alone | `tether_arbiter_step()` in [`core/src/arbiter.c`](../core/src/arbiter.c) |
| Safety monitor | Judges one AI proposal against the envelope | `tether_monitor_check()` |
| Switch | AI ↔ SAFE with hysteresis: readmit only at a breath boundary after N healthy proposals | `tether_arbiter_step()` |
| Recovery controller | A plain pressure-control breath whose bounds are verified before start-up | `tether_recovery_command()`, `tether_config_validate()` |

The core is pure computation (no I/O, no allocation, no blocking), so the same code runs in the host
simulator, in the unit tests and inside the QNX arbiter.

## The envelope

A proposal is accepted only if all of this holds. Each failure has a name that shows up in the status
and on the dashboard.

| Reason | Meaning |
|---|---|
| `no-proposal`, `stale` | The AI has not proposed, or its last proposal is older than the freshness deadline |
| `disconnected` | The kernel reported the AI process gone |
| `non-finite` | NaN or infinity |
| `range` | Outside the pressure limits |
| `rise` | Pressure would rise faster than allowed between two AI updates |
| `expiration-pressure` | Pressure above PEEP while the patient breathes out |
| `predicted-overdistension` | Lookahead through the lung model says this command, held to the end of the inspiration, would push the volume past its limit. Compliance is multiplied by a margin above 1 and resistance by one below 1, so the prediction errs on the cautious side |
| `volume-limit`, `measured-pressure` | The measured state already broke a limit, whoever was driving |

The lookahead uses the estimator, which is fed only by the sensors. A misbehaving AI cannot talk the
monitor into a looser envelope.

The recovery controller is checked statically. `tether_config_validate()` refuses a configuration in
which the recovery pressure, held forever on the most compliant lung the envelope allows, could
deliver more than the volume limit or leave the pressure limits.

## Processes

| Process | Runs as | Job |
|---|---|---|
| `tetherd` | root | Starts the others in order, restarts a crashed AI, kills a hung one |
| `tether-hw` | root | The virtual hardware: a resource manager serving the blower, the sensor and the lung |
| `tether-arbiter` | root | The control loop and the monitor |
| `tether-ai` | uid 2002, abilities locked | The neural network |
| `tether-bridge` | uid 2003, group 2000 | HTTP gateway and dashboard |

Why separate processes: an address space is the isolation the kernel can enforce. The AI cannot
overwrite the arbiter's memory, and it cannot open the actuator. It can only send messages.

### Direction of the messages

```
AI      --MsgSend(PROPOSE)--> arbiter      reply: the current observation
bridge  --MsgSend(STATUS | TELEMETRY | GUARD | RESET)--> arbiter
bridge  --MsgSend(FAULT | MODEL)--> AI
arbiter --devctl--> /dev/tether/sensor, /dev/tether/actuator
bridge  --devctl--> /dev/tether/plant
```

The AI is always the client. If the arbiter were the AI's client it could block on a hung AI. As a
server it only ever replies. Replies carry their own length as the status, because `MsgSend()`
returns the status given to `MsgReply()`.

The wire format is in [`core/include/tether/proto.h`](../core/include/tether/proto.h).

### Threads and priorities

| Priority | Thread | Why there |
|---|---|---|
| 60 | arbiter control loop | Outranks everything the AI can reach |
| 58, 56 | hardware resource manager, physics | Answer the control loop without waiting for anything below |
| 50 | arbiter IPC server | Serves the AI and the dashboard |
| 40 | `tetherd` | Can always act on a hung AI |
| 35 | arbiter logger | Does the arithmetic and the disk writes the control loop must not |
| 25 | dashboard gateway | Above the AI, so the dashboard still works while the AI saturates the CPUs |
| 22, 20 | AI control thread, AI inference and hog threads | The control thread outranks the hogs so a fault can always be cleared |

The control loop publishes its state to the other threads through a seqlock snapshot. Readers copy
and retry; the writer never waits. The hardware's lock inherits priority.

### Timing

A one-shot timer is armed at an absolute time on a fixed grid after each tick, so the wake-up
latency we report is scheduling jitter and does not accumulate. If the loop falls more than ten
periods behind, it gives up catching up and counts the skipped ticks (`ctl_overruns`), so overload is
visible instead of hidden.

The hardware has its own watchdog. If no command arrives for `hw_watchdog_s`, the blower drops to
PEEP by itself. A dead arbiter cannot leave the patient pressurised.

## What each fault shows

| Fault | What happens | Who notices |
|---|---|---|
| `spike`, `nan` | Out-of-range or non-finite output | The monitor, on the next tick |
| `drift` | A bias that grows each breath | The lookahead, before the volume limit |
| `stuck` | Repeats an inspiratory value into expiration | The monitor (`expiration-pressure`) |
| `slow` | Inference takes 40 ms | The freshness deadline |
| `crash` | Write through NULL | The kernel's disconnect pulse; then `tetherd` restarts it |
| `hang` | Spins forever | The freshness deadline, then `tetherd` kills it after a second of silence |
| `hog` | Saturates every CPU at the AI's priority | Nobody has to: the scheduler keeps the control loop running |
| `bypass` | Tries to open the actuator | The kernel (`EACCES`); the hardware counts it |

## Hot-swapping the model

The envelope is fixed and validated; the model behind it can change. `tether-ai` loads a new `.tmlp`
into a spare slot and swaps one index. `models/vent-unvalidated.tmlp` is a model trained to overshoot
by 12 cmH₂O: swapped in at run time it is rejected by the same envelope, with no violation. This is
the engineering idea behind a predetermined change control plan: change the learned part without
reopening the verified part.
