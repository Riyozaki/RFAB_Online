#!/usr/bin/env python3
"""Cross-checks server settings between the code and the documentation.

Every Console::Setting in the server sources must be documented in
docs/configuration.md, and every setting documented there must exist in the code.
Drift in either direction is how a fork ends up with "features" nobody can find
and documentation for switches that no longer exist.

Usage: tools/check_settings.py [--root DIR]
Exit code: 0 when consistent, 1 otherwise.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Console::Setting uName{"Section:Key", "description", default};
SETTING_RE = re.compile(
    r'Console::Setting(?:<[^>]+>)?\s+\w+\s*[{(]\s*(?:\n\s*)?"([A-Za-z]+:[A-Za-z0-9_]+)"'
)
# extern Console::Setting x; redeclarations are fine, they are not new settings.
EXTERN_RE = re.compile(r'extern\s+Console::Setting')
# | `Section:Key` | ... |
DOC_RE = re.compile(r'^\|\s*`([A-Za-z]+:[A-Za-z0-9_]+)`\s*\|', re.MULTILINE)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default=str(Path(__file__).resolve().parent.parent))
    parser.add_argument("--docs", default="docs/configuration.md")
    args = parser.parse_args()

    root = Path(args.root)
    docs_path = root / args.docs

    code_settings: dict[str, str] = {}
    for path in sorted((root / "Code" / "server").rglob("*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        # extern redeclarations do not create a setting, only definitions do
        text = EXTERN_RE.sub("/*extern*/ ", text)
        for match in SETTING_RE.finditer(text):
            name = match.group(1)
            relative = path.relative_to(root).as_posix()
            if name in code_settings and code_settings[name] != relative:
                print(f"DUPLICATE setting {name}: {code_settings[name]} and {relative}", file=sys.stderr)
            code_settings.setdefault(name, relative)

    if not docs_path.exists():
        print(f"documentation not found: {docs_path}", file=sys.stderr)
        return 1

    doc_settings = set(DOC_RE.findall(docs_path.read_text(encoding="utf-8", errors="replace")))

    missing_in_docs = sorted(set(code_settings) - doc_settings)
    missing_in_code = sorted(doc_settings - set(code_settings))

    for name in missing_in_docs:
        print(f"NOT DOCUMENTED: {name} ({code_settings[name]})", file=sys.stderr)
    for name in missing_in_code:
        print(f"NOT IN CODE:    {name} (documented in {args.docs})", file=sys.stderr)

    print(f"settings in code: {len(code_settings)}, documented: {len(doc_settings)}, "
          f"undocumented: {len(missing_in_docs)}, stale: {len(missing_in_code)}")

    return 0 if not missing_in_docs and not missing_in_code else 1


if __name__ == "__main__":
    sys.exit(main())
