# Tether — agent notes

Runtime assurance for AI controllers on QNX 8.0 (QNX Global Hackathon 2026, category Robotics & Medical).

- `core/` portable C11, no I/O, no allocation in the control path. Builds on macOS/Linux and QNX.
- `host/` closed-loop simulator + datagen + CLI (`tether-sim`). `tests/` ctest suite (`test_core`).
- `qnx/` the QNX-only processes (resource managers, arbiter, AI, bridge). Only built with the QNX toolchain.
- `tools/train/train.py` trains the MLP (NumPy). `models/*.tmlp` are the shipped models.
- `docs/` is public. `private/` is gitignored (Vietnamese strategy/submission notes) — never commit it.

Commits: `type(scope): plain-English sentence`. Build and test on the host:
`cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`.
Never claim medical certification. The lung is simulated; say so wherever results are shown.
