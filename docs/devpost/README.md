# Devpost images

Images for the project page. Each is rendered from an HTML file in [`src/`](src), so the text and
numbers can be edited and the picture rebuilt:

```
tools/gallery/render.sh              # all of them
tools/gallery/render.sh 03-containment
```

It needs Chrome (or set `CHROME` to a Chromium binary). The three dashboard screenshots in
[`src/screens/`](src/screens) are captures from the running QNX image.

## Gallery (1800 × 1200, 3:2)

Upload in this order.

| File | Shows | Numbers come from |
|---|---|---|
| `01-hero.png` | Title, one-line pitch, the dashboard while the AI asks for 70 cmH₂O | live capture |
| `02-architecture.png` | Processes, who talks to whom, what the kernel refuses | [`docs/architecture.md`](../architecture.md) |
| `03-containment.png` | Peak volume with and without Tether, per fault | host simulation, `tether-sim --campaign` |
| `04-qnx-evidence.png` | Nine QNX features, each with an observed result | `tools/qemu/selftest.sh`, logs |
| `05-kernel-says-no.png` | Real terminal output: the AI's user is refused | live QNX shell |
| `06-timing.png` | Control-loop wake-up latency under four stresses | `tools/qemu/latency-test.sh` |
| `07-hot-swap.png` | An unvalidated model swapped in: AI alone vs behind Tether | host simulation + live capture |
| `08-the-ai.png` | The neural network, how it was trained, one lesson learned | `models/`, `tools/train/` |
| `09-dashboard.png` | The dashboard and what can be broken on purpose | live capture |

## Thumbnails

| File | Size | Use |
|---|---|---|
| `thumb-devpost-3x2.png` | 1500 × 1000 (3:2) | Project thumbnail |
| `video-thumbnail-16x9.png` | 1280 × 720 (16:9) | Video thumbnail (YouTube and similar) |

Devpost accepts JPG, PNG or GIF up to 5 MB and recommends 3:2. Check the limits on the submission
form before uploading; every file here is well under 1 MB.

## Honesty

Every slide that shows a clinical-looking signal says the patient is simulated. The timing slide
says the numbers come from QEMU under software emulation. The containment and hot-swap tables are
host simulations of the same arbiter code that runs on QNX, and say so.
