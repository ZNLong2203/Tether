#!/usr/bin/env python3
"""Train the ventilator policy (a small MLP) from tether-datagen output.

NumPy only, so it runs anywhere. Writes a ".tmlp" file (layout documented in
core/include/tether/policy.h) that the QNX AI process loads at start-up and
on hot-swap.

    tether-datagen build/dataset.f32 300
    python3 tools/train/train.py build/dataset.f32 models/vent-v1.tmlp --model-id 1
"""
import argparse
import struct
import sys

import numpy as np

N_IN = 8
OUT_SCALE = 10.0  # network predicts dp / 10


def load(path):
    raw = np.fromfile(path, dtype=np.float32)
    if raw.size % (N_IN + 1):
        sys.exit("dataset size is not a multiple of %d floats" % (N_IN + 1))
    d = raw.reshape(-1, N_IN + 1)
    return d[:, :N_IN].astype(np.float64), d[:, N_IN].astype(np.float64)


def init_layer(rng, n_in, n_out):
    lim = np.sqrt(6.0 / (n_in + n_out))
    return rng.uniform(-lim, lim, (n_in, n_out)), np.zeros(n_out)


def forward(params, x):
    acts = [x]
    h = x
    for i, (w, b) in enumerate(params):
        h = h @ w + b
        if i < len(params) - 1:
            h = np.tanh(h)
        acts.append(h)
    return acts


def train(x, y, hidden, epochs, batch, lr, seed):
    rng = np.random.default_rng(seed)
    sizes = [N_IN] + hidden + [1]
    params = [init_layer(rng, sizes[i], sizes[i + 1]) for i in range(len(sizes) - 1)]
    m = [(np.zeros_like(w), np.zeros_like(b)) for w, b in params]
    v = [(np.zeros_like(w), np.zeros_like(b)) for w, b in params]
    n = x.shape[0]
    idx = np.arange(n)
    step = 0
    for ep in range(epochs):
        rng.shuffle(idx)
        cur_lr = lr * (0.5 * (1 + np.cos(np.pi * ep / epochs)) * 0.95 + 0.05)
        total = 0.0
        for s in range(0, n, batch):
            bi = idx[s:s + batch]
            xb, yb = x[bi], y[bi][:, None]
            acts = forward(params, xb)
            err = acts[-1] - yb
            total += float((err ** 2).sum())
            grad = 2.0 * err / len(bi)
            step += 1
            for li in reversed(range(len(params))):
                w, b = params[li]
                gw = acts[li].T @ grad
                gb = grad.sum(axis=0)
                if li > 0:
                    grad = (grad @ w.T) * (1.0 - acts[li] ** 2)
                for (p, mm, vv, g) in ((w, m[li][0], v[li][0], gw), (b, m[li][1], v[li][1], gb)):
                    mm *= 0.9
                    mm += 0.1 * g
                    vv *= 0.999
                    vv += 0.001 * g * g
                    mh = mm / (1 - 0.9 ** step)
                    vh = vv / (1 - 0.999 ** step)
                    p -= cur_lr * mh / (np.sqrt(vh) + 1e-8)
        if ep % 5 == 0 or ep == epochs - 1:
            print("epoch %3d  rmse %.4f (normalised, x%.0f = cmH2O)" % (ep, np.sqrt(total / n), OUT_SCALE))
    return params


def write_tmlp(path, params, mean, std, model_id):
    with open(path, "wb") as f:
        f.write(b"TMLP")
        f.write(struct.pack("<III", 1, model_id, N_IN))
        f.write(mean.astype("<f4").tobytes())
        f.write(std.astype("<f4").tobytes())
        f.write(struct.pack("<I", len(params)))
        for i, (w, b) in enumerate(params):
            n_in, n_out = w.shape
            act = 1 if i < len(params) - 1 else 0
            f.write(struct.pack("<III", n_in, n_out, act))
            f.write(w.T.astype("<f4").tobytes())  # row-major [out][in]
            f.write(b.astype("<f4").tobytes())
        f.write(struct.pack("<ff", OUT_SCALE, 0.0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dataset")
    ap.add_argument("output")
    ap.add_argument("--model-id", type=int, default=1)
    ap.add_argument("--hidden", type=int, nargs="+", default=[32, 32])
    ap.add_argument("--epochs", type=int, default=40)
    ap.add_argument("--batch", type=int, default=1024)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--bias", type=float, default=0.0,
                    help="add this many cmH2O to every output (used to build a deliberately bad model for the hot-swap demo)")
    a = ap.parse_args()

    x, y = load(a.dataset)
    print("loaded %d rows" % len(x))
    mean = x.mean(axis=0)
    std = x.std(axis=0)
    std[std < 1e-6] = 1.0
    xn = (x - mean) / std
    params = train(xn, y / OUT_SCALE, a.hidden, a.epochs, a.batch, a.lr, a.seed)
    acts = forward(params, xn[:50000])
    pred = acts[-1][:, 0] * OUT_SCALE
    print("final rmse on a 50k sample: %.3f cmH2O, max abs %.2f" %
          (np.sqrt(np.mean((pred - y[:50000]) ** 2)), np.abs(pred - y[:50000]).max()))
    if a.bias:
        print("adding output bias %.2f cmH2O" % a.bias)
    # fold the optional bias into the last layer
    w, b = params[-1]
    params[-1] = (w, b + a.bias / OUT_SCALE)
    write_tmlp(a.output, params, mean, std, a.model_id)
    print("wrote", a.output)


if __name__ == "__main__":
    main()
