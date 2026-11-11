#!/usr/bin/env python3
"""Build the codelab from its template so the code in the text is the code that was compiled and run.

    tools/codelab/assemble.py

Reads docs/codelab/qnx-kernel-gate.template.md, replaces every @@src/<file>@@ with the file's
contents from docs/codelab/markdown/qnx-kernel-gate/src/, and writes
docs/codelab/markdown/qnx-kernel-gate/qnx-kernel-gate.md.
"""
import pathlib, re, sys

root = pathlib.Path(__file__).resolve().parents[2]
base = root / "docs" / "codelab"
folder = base / "markdown" / "qnx-kernel-gate"
text = (base / "qnx-kernel-gate.template.md").read_text()

def include(match):
    path = folder / match.group(1)
    if not path.is_file():
        sys.exit(f"missing {path}")
    return path.read_text().rstrip("\n")

out = re.sub(r"@@(src/[A-Za-z0-9_.\-]+)@@", include, text)
(folder / "qnx-kernel-gate.md").write_text(out)
lines = out.count("\n") + 1
print(f"wrote {folder / 'qnx-kernel-gate.md'} ({lines} lines)")
