# Running Tether

Two ways: the portable core on any computer, and the full system on QNX 8.0.

## 1. The core and the simulator (no QNX)

Needs a C11 compiler and CMake 3.16 or newer. No other dependency.

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure          # 775 checks
build/host/tether-sim --model models/vent-v1.tmlp --campaign
build/host/tether-sim --model models/vent-v1.tmlp --fault spike --at 10 --csv spike.csv
build/host/tether-sim --model models/vent-unvalidated.tmlp --ctrl raw      # the biased model with no arbiter
build/host/tether-sim --model models/vent-unvalidated.tmlp --ctrl ai       # the same model behind the arbiter
```

Retraining a model needs Python 3 and NumPy:

```
build/host/tether-datagen dataset.bin
python3 tools/train/train.py dataset.bin models/mine.tmlp --model-id 4 --hidden 24 24
```

## 2. The full system on QNX 8.0

This was built on an Apple M4 Mac with 16 GB, where nothing QNX-specific runs natively: QNX SDP 8.0
supports only x86 Linux and Windows hosts. The tools below put the QNX host tools in an x86_64 Linux
container and run the target in QEMU. On an x86 Linux machine you can skip the container.

### Prerequisites

* Docker (Docker Desktop on macOS), and `brew install qemu` (QEMU 11 was used).
* A free **QNX Everywhere** licence for SDP 8.0, non-commercial use:
  1. Create a myQNX account and request the licence at <https://www.qnx.com/getqnx>.
  2. When it appears in the License Manager (<https://www.qnx.com/account/dashboard/>), click
     *Accept License*, then deploy it to your own e-mail address.
  3. Download QNX Software Center **2.0.4 for Linux** (`qnx-setup-2.0.4-…-linux.run`) from the QNX
     download centre. The macOS build, QSC 1.7, installs only SDP 7.x.
* A key for the VM, once: `ssh-keygen -t ed25519 -N '' -C tether-vm -f ~/.qnx/tether_vm_ed25519`.

### Steps

```
tools/docker/build-image.sh                                 # the build container (ubuntu 24.04, linux/amd64)
tools/docker/install-sdp.sh ~/Downloads/qnx-setup-2.0.4-…-linux.run
tools/docker/install-sdp.sh --packages com.qnx.qnx800.target.qemuvirt,com.qnx.qnx800.target.driver.virtio,com.qnx.qnx800.target.driver.virtio.devb,com.qnx.qnx800.target.driver.virtio.devc,com.qnx.qnx800.target.driver.virtio.startup,com.qnx.qnx800.target.net.devsvirtio
tools/qemu/build-image.sh                                   # cross-compiles, then builds the image
tools/qemu/run-vm.sh start                                  # boots in the background
open http://localhost:8080                                  # about 12 s after boot
```

`install-sdp.sh` is interactive on purpose: it shows the QNX licence agreement for you to accept and
asks for your myQNX password, which it keeps only in a memory-backed file inside the container.

The SDP is installed in `~/qnx800`, QNX Software Center in `~/qnx-qsc`.

### Everyday commands

| Command | What it does |
|---|---|
| `tools/qemu/run-vm.sh ssh` | Root shell in the VM (key login) |
| `tools/qemu/run-vm.sh` | Foreground, serial console on the terminal (quit: `Ctrl-A x`) |
| `tools/qemu/run-vm.sh stop` | Stop the VM |
| `tools/build-qnx.sh` | Cross-compile only (output in `build-qnx/`) |
| `tools/qemu/deploy.sh` | Copy a fresh build into the running VM without rebuilding the image |
| `tools/qemu/stack.sh start\|stop\|status\|logs` | Restart the Tether processes in the VM |
| `tools/qemu/selftest.sh` | The 21 pass/fail checks |
| `tools/qemu/fault-tour.sh [fault …]` | Inject faults one by one and print what the arbiter did |
| `tools/qemu/latency-test.sh 25 none hog bypass slow` | Wake-up latency in separate windows |

Inside the VM, `/data/tether/bin/tether-ctl` talks to the system:

```
tether-ctl status              arbiter status as JSON
tether-ctl hw                  hardware status (watchdog, refused opens, lung)
tether-ctl fault spike         make the AI misbehave (also: nan drift stuck slow hog crash hang bypass none)
tether-ctl model vent-unvalidated
tether-ctl lung ards           change the simulated patient
tether-ctl guard off           demo: pass the AI straight to the blower
slog2info | grep tether        the system log
```

### Options

All Tether processes read the same options from `/data/tether/flags`
(default in [`qnx/image/tether.flags`](../qnx/image/tether.flags)):

```
--control-hz=N --ai-hz=N --plant-hz=N --max-age-ms=N --readmit-ticks=N --hw-watchdog-ms=N
```

`tether_config_validate()` refuses a combination that cannot work, for example a proposal freshness
shorter than one AI period, or a recovery pressure that could exceed the volume limit.

### Troubleshooting

* `QNX_HOST/QNX_TARGET not set`: run builds through `tools/build-qnx.sh`, which sources the SDP.
* `Host file 'startup-qemu-virt' not available` while building the image: the QEMU BSP package from
  the `--packages` step above is missing.
* QEMU stops with `Assertion failed: (isv)`: you asked for the `hvf` accelerator. Use TCG.
* sshd reports `/usr/libexec/sshd-auth does not exist`: the image was built without
  [`qnx/image/snippets`](../qnx/image/snippets).
* `mkqnximage` writes private keys into `target/`. It is git-ignored; never add it.
