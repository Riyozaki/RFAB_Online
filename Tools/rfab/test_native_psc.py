#!/usr/bin/env python3
"""Declaration/PEX ABI checks; not a Skyrim VM execution test."""
import re
import struct
import unittest
from pathlib import Path

import compile_native_psc as native

ROOT = Path(__file__).resolve().parents[2]


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def n(self, fmt):
        result, = struct.unpack_from(">" + fmt, self.data, self.at)
        self.at += struct.calcsize(">" + fmt)
        return result

    def string(self):
        size = self.n("H")
        text = self.data[self.at:self.at + size].decode("utf-8")
        self.at += size
        return text


def decode_declarations(data):
    r = Reader(data)
    assert r.n("I") == 0xFA57C0DE
    assert (r.n("B"), r.n("B"), r.n("H")) == (3, 2, 1)
    assert r.n("Q") == 0
    source, user, computer = r.string(), r.string(), r.string()
    strings = [r.string() for _ in range(r.n("H"))]
    text = lambda: strings[r.n("H")]
    assert r.n("B") == 0  # no debug section
    assert r.n("H") == 1 and text() == "hidden" and r.n("B") == 0
    assert r.n("H") == 1  # one object
    name = text()
    size = r.n("I")
    end = r.at + size
    assert text() == "" and text() == ""
    flags = r.n("I")
    assert flags in (0, 1) and text() == ""  # optional Hidden; auto-state
    assert r.n("H") == 0 and r.n("H") == 0  # no variables/properties
    assert r.n("H") == 1 and text() == ""  # one default state
    methods = []
    for _ in range(r.n("H")):
        method, result = text(), text()
        assert text() == "" and r.n("I") == 0 and r.n("B") == 3
        params = tuple((text(), text()) for _ in range(r.n("H")))
        assert r.n("H") == 0 and r.n("H") == 0
        methods.append(native.Function(method, result, params))
    assert r.at == end == len(data)
    assert (source, user, computer) == (name + ".psc", "RFAB", "reproducible")
    return native.Script(name, bool(flags), tuple(methods))


class NativeDeclarationTests(unittest.TestCase):
    def test_all_shipped_reward_native_signatures_and_registration_counts_agree(self):
        path = ROOT / "GameFiles/Skyrim/scripts/source/RFABCoopRewards.psc"
        script = native.parse(path.read_text())
        self.assertEqual(script, decode_declarations(native.emit(script)))
        cpp = (ROOT / "Code/client/Games/Skyrim/Misc/RewardPapyrus.cpp").read_text()
        bound = {name: int(count) for name, count in re.findall(r'RewardFunction::\w+, "(\w+)", (\d+), V::k\w+', cpp)}
        self.assertEqual({f.name: len(f.parameters) for f in script.functions}, bound)
        expected = {
            "RegisterAdapter": ("bool", ("Form", "string", "string", "int", "bool", "string")),
            "GetWorldEpoch": ("string", ()), "GetMode": ("int", ()),
            "Submit": ("int", ("string", "int", "int", "int", "int", "int", "string", "string")),
            "Complete": ("bool", ("string", "int", "int", "int")),
            "Claim": ("bool", ("string",)), "CanApply": ("bool", ("string",)),
        }
        self.assertEqual({f.name: (f.result, tuple(t for _, t in f.parameters)) for f in script.functions}, expected)

    def test_checked_in_native_pex_is_reproducible(self):
        folder = ROOT / "GameFiles/Skyrim/scripts"
        expected = native.emit(native.parse((folder / "source/RFABCoopRewards.psc").read_text()))
        self.assertEqual((folder / "RFABCoopRewards.pex").read_bytes(), expected)

    def test_no_game_behavior_can_be_compiled_by_declaration_emitter(self):
        for text in ("Function Award()", "int x = 1", "Event OnInit()", "int Property Wallet Auto",
                     "int Function Award() global", "int Function Award() native", "EndFunction",
                     "int Function Award(int x = 0) global native", "int[] Function Get() global native",
                     "int Function Award(MadeUpType x) global native"):
            with self.subTest(text=text), self.assertRaises(native.InvalidSource):
                native.parse("Scriptname Test\n" + text)
        with self.assertRaises(native.InvalidSource):
            native.parse("Scriptname Test extends Quest\nint Function Foo() global native")

    def test_names_and_eight_scalar_parameters_are_bounded(self):
        for args in (", ".join(f"int p{i}" for i in range(9)), "int X, int x", "int ../x"):
            with self.assertRaises(native.InvalidSource):
                native.parse(f"Scriptname Test\nint Function F({args}) global native")
        with self.assertRaises(native.InvalidSource):
            native.parse("Scriptname Test\nint Function f() global native\nint Function F() global native")
        with self.assertRaises(native.InvalidSource):
            native.parse("Scriptname Test\n" + ";" * 65536)

    def test_comments_bom_case_and_hidden_are_structural_not_game_instructions(self):
        a = native.parse("\ufeff Scriptname Test Hidden ; docs\n INT FUNCTION Name(BoOl Arg) GLOBAL NATIVE")
        self.assertEqual(a, decode_declarations(native.emit(a)))
        self.assertEqual(native.emit(a), native.emit(a))
        self.assertEqual(a.functions[0].parameters, (("Arg", "bool"),))


if __name__ == "__main__":
    unittest.main()
