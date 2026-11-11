# Codelab: Keep an Untrusted Process Away from a Device on QNX 8.0

A hands-on tutorial built from the pieces of Tether, in the format of
[qnx.github.io/codelabs](https://qnx.github.io/codelabs/): a Markdown file with a metadata header, one
`##` heading per step and a `Duration:` line under each. It was submitted to QNX as pull request
[qnx/codelabs#73](https://github.com/qnx/codelabs/pull/73).

It teaches one thing: put a process you do not trust behind a gate that the QNX kernel enforces. It
builds a resource manager, shows the kernel refusing an ordinary user, locks a process down with
`procmgr_ability()`, learns when a client dies from a disconnect pulse, and explains why a
`TimerTimeout()` deadline does not always release a client. About 55 minutes; every command and output
in it was run on QNX SDP 8.0.

| Path | What |
|---|---|
| [`markdown/qnx-kernel-gate/qnx-kernel-gate.md`](markdown/qnx-kernel-gate/qnx-kernel-gate.md) | The codelab |
| [`markdown/qnx-kernel-gate/src/`](markdown/qnx-kernel-gate/src) | The seven example files and `build.sh` |
| [`qnx-kernel-gate.template.md`](qnx-kernel-gate.template.md) | The text with `@@src/...@@` markers where code goes |
| [`../../tools/codelab/assemble.py`](../../tools/codelab/assemble.py) | Fills the markers, so the code in the text is exactly the code that was compiled |

Edit the template or the sources, then run `tools/codelab/assemble.py`. Do not edit the generated
`.md` by hand.

The Markdown follows the structure of QNX's own examples, but it has not been rendered with `claat`
yet, so layout details may differ once QNX publishes it.

## Re-running the examples

```
tools/docker/run.sh bash -lc 'source /opt/qnx800/qnxsdp-env.sh && cd /work/docs/codelab/markdown/qnx-kernel-gate/src && ./build.sh'
```

then copy `out/*` to the target as the codelab describes.
