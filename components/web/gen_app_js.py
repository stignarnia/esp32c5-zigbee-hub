#!/usr/bin/env python3
"""Build step: put the example scripts (examples/*.lua) into the UI's app.js.

app.js contains `const EXAMPLES = /*@EXAMPLES@*/ {};`; the marker and the `{}` after it
are replaced with an object of every examples/*.lua file, keyed by a name made from the
file name ("door-alarm.lua" -> "Door alarm"), in file-name order. The result goes to the
build directory and is gzipped into the firmware like the other UI files. The examples
are only offered in the editor; nothing is stored on the hub until the user saves.

Usage: gen_app_js.py <app.js> <examples dir> <output app.js>
"""
import json
import pathlib
import sys

MARKER = "/*@EXAMPLES@*/ {}"


def main() -> int:
    src, ex_dir, out = (pathlib.Path(a) for a in sys.argv[1:4])
    js = src.read_text(encoding="utf-8")
    if js.count(MARKER) != 1:
        print(f"gen_app_js: '{MARKER}' must appear exactly once in {src}", file=sys.stderr)
        return 1
    examples = {}
    for f in sorted(ex_dir.glob("*.lua")):
        name = f.stem.replace("-", " ").replace("_", " ")
        examples[name[:1].upper() + name[1:]] = f.read_text(encoding="utf-8")
    if "Template" not in examples:
        print(f"gen_app_js: {ex_dir}/template.lua is missing (the editor starts from it)", file=sys.stderr)
        return 1
    obj = json.dumps(examples, ensure_ascii=False, indent=1)
    out.write_text(js.replace(MARKER, obj), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
