# Codelab: Keep an Untrusted Process Away from a Device on QNX 8.0

A hands-on tutorial for the **Best Codelab / Tutorial** bonus prize of the QNX Global Hackathon. It is
written in the format of [qnx.github.io/codelabs](https://qnx.github.io/codelabs/): a Markdown file with
a metadata header, one `##` heading per step and a `Duration:` line under each.

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

## What the prize asks for

From the hackathon's official rules as quoted in public listings (the Devpost rules page itself would
not load for our tools, so read it yourself before submitting):

- Open to any entrant who also submits an eligible project.
- You provide the **URL of a codelab or tutorial** that you wrote **during the Submission Period** and
  that teaches a reader how to do a task **on or for QNX 8.0**. The URL goes in the designated field of
  the submission form.
- The Submission Period runs from 2 November to 15 December 2026.
- It does not have to be about your hackathon project.

## Status

Pull request opened: <https://github.com/qnx/codelabs/pull/73>. It was **not** previewed with `claat` before opening (the build tool was not run), so look at the rendered page once it is available and push fixes to the same branch if needed.

## How to publish it

QNX publishes codelabs from the public repository
[github.com/qnx/codelabs](https://github.com/qnx/codelabs) through pull requests. Their guide says to
change **only** `markdown/<codelab-name>/`; anything else gets the pull request rejected.

```
# 1. Fork qnx/codelabs on GitHub, then:
git clone https://github.com/<you>/codelabs.git
cd codelabs
git checkout -b qnx-kernel-gate

# 2. Copy the codelab folder, without the build output
rsync -a --exclude out <this repo>/docs/codelab/markdown/qnx-kernel-gate/ markdown/qnx-kernel-gate/

# 3. Push and open the pull request
git add markdown/qnx-kernel-gate
git commit -m "Add codelab: keep an untrusted process away from a device"
git push -u origin qnx-kernel-gate
gh pr create --repo qnx/codelabs --title "Add codelab: keep an untrusted process away from a device on QNX 8.0"
```

The guide says maintainers "will review and add" pull requests and gives no turnaround. We could not
find out whether the prize needs the codelab to be merged. **Do not wait for the merge**: put a URL in
the Devpost field that already works, for instance the pull request, or the rendered file in your own
public repository, and replace it with `https://qnx.github.io/codelabs/qnx-kernel-gate/index.html`
(the usual address pattern) once it is merged, if the form still allows editing.

## Preview it

The QNX fork of Google's `claat` renders it. It needs Go:

```
git clone https://github.com/qnx/tools.git && cd tools/claat && go install
cd <qnx/codelabs fork>
claat export -f html -o docs ./markdown/qnx-kernel-gate/qnx-kernel-gate.md
claat serve qnx-kernel-gate.md        # http://localhost:8000
```

This has **not** been run for this codelab: the Markdown follows the structure of QNX's own examples,
but nobody has seen it rendered yet. Preview it before opening the pull request.

## Re-running the examples

```
tools/docker/run.sh bash -lc 'source /opt/qnx800/qnxsdp-env.sh && cd /work/docs/codelab/markdown/qnx-kernel-gate/src && ./build.sh'
```

then copy `out/*` to the target as the codelab describes.
