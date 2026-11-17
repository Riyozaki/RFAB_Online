#!/usr/bin/env python3
"""Deterministic Skyrim 3.2 PEX emitter for *declarations-only* native API scripts.

Not a Papyrus compiler: any executable code, inheritance, properties, default arguments,
arrays or non-global/native function is refused. Installed RFAB scripts need their real
compiler + imports; this tool must never be used to recompile/replace those scripts.
Format cross-checked with the existing SkyrimTogetherUtils.pex and Caprica's PexFile,
PexObject, PexState, PexFunction and PexFunctionParameter at e4dee0860914d75e770d3f9ab374f7aba474b701.
No compiler binaries, game SDK or third-party source are redistributed.
"""
from __future__ import annotations
import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import struct


class InvalidSource(ValueError):
    pass


@dataclass(frozen=True)
class Function:
    name: str
    result: str
    parameters: tuple[tuple[str, str], ...]  # name, type


@dataclass(frozen=True)
class Script:
    name: str
    hidden: bool
    functions: tuple[Function, ...]


IDENT = r"[A-Za-z_][A-Za-z_0-9]{0,63}"
TYPES = {v.casefold(): v for v in ("bool", "int", "float", "string", "Form", "Actor", "Quest")}


def parse(source: str) -> Script:
    if len(source.encode("utf-8")) > 65536:
        raise InvalidSource("native declaration source exceeds 64 KiB")
    lines = [s.split(";", 1)[0].strip() for s in source.lstrip("\ufeff").splitlines()]
    lines = [s for s in lines if s]
    if not lines:
        raise InvalidSource("empty native declaration source")
    header = re.fullmatch(rf"Scriptname\s+({IDENT})(\s+Hidden)?", lines[0], re.I)
    if not header:
        raise InvalidSource("only an optional Hidden flag is allowed on a declarations-only Scriptname")
    functions = []
    names = set()
    for line in lines[1:]:
        match = re.fullmatch(rf"({IDENT})\s+Function\s+({IDENT})\s*\(([^()]*)\)\s+global\s+native", line, re.I)
        if not match or match[1].casefold() not in TYPES:
            raise InvalidSource(f"not a supported global native declaration: {line}")
        name = match[2]
        if name.casefold() in names:
            raise InvalidSource(f"duplicate native function: {name}")
        names.add(name.casefold())
        parameters = []
        param_names = set()
        for text in match[3].split(",") if match[3].strip() else []:
            param = re.fullmatch(rf"\s*({IDENT})\s+({IDENT})\s*", text)
            if not param or param[1].casefold() not in TYPES or param[2].casefold() in param_names:
                raise InvalidSource(f"invalid/duplicate native parameter in {line}")
            param_names.add(param[2].casefold())
            parameters.append((param[2], TYPES[param[1].casefold()]))
        if len(parameters) > 8:
            raise InvalidSource("native dispatcher accepts at most eight scalar parameters")
        functions.append(Function(name, TYPES[match[1].casefold()], tuple(parameters)))
    if not functions or len(functions) > 64:
        raise InvalidSource("a declarations-only script needs 1..64 native functions")
    return Script(header[1], bool(header[2]), tuple(functions))


def emit(script: Script) -> bytes:
    strings: list[str] = []

    def index(value: str) -> bytes:
        if value not in strings:
            strings.append(value)
        return struct.pack(">H", strings.index(value))

    empty = index("")
    hidden = index("hidden")
    object_name = index(script.name)
    functions = bytearray()
    for function in script.functions:
        functions += index(function.name) + index(function.result) + empty
        functions += struct.pack(">IBH", 0, 3, len(function.parameters))  # no user flags; global | native
        for name, kind in function.parameters:
            functions += index(name) + index(kind)
        functions += struct.pack(">HH", 0, 0)  # no locals or executable instructions
    body = empty + empty + struct.pack(">I", int(script.hidden)) + empty
    body += struct.pack(">HHH", 0, 0, 1)  # no variables/properties; one default state
    body += empty + struct.pack(">H", len(script.functions)) + functions

    def text(value: str) -> bytes:
        data = value.encode("utf-8")
        return struct.pack(">H", len(data)) + data

    output = bytearray(struct.pack(">IBBHQ", 0xFA57C0DE, 3, 2, 1, 0))
    output += text(script.name + ".psc") + text("RFAB") + text("reproducible")
    output += struct.pack(">H", len(strings)) + b"".join(text(s) for s in strings)
    output += struct.pack(">BH", 0, 1) + hidden + b"\0"  # no debug; hidden user-flag is bit0
    output += struct.pack(">H", 1) + object_name + struct.pack(">I", len(body)) + body
    return bytes(output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--check", action="store_true", help="verify the checked-in PEX exactly matches its native declarations")
    args = parser.parse_args()
    try:
        script = parse(args.source.read_text(encoding="utf-8-sig"))
        if args.source.stem.casefold() != script.name.casefold() or args.output.stem.casefold() != script.name.casefold():
            raise InvalidSource("source and output filenames must match Scriptname")
        output = emit(script)
        if args.check:
            if args.output.read_bytes() != output:
                raise InvalidSource("stale/non-reproducible native PEX; regenerate it from the checked-in PSC")
        else:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(output)
        print(f"native declarations: {script.name}, {len(script.functions)} functions, {len(output)} bytes, {'verified' if args.check else 'written'}")
        return 0
    except (ValueError, OSError) as error:
        parser.exit(1, f"native declarations: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
