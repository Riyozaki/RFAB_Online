#!/usr/bin/env python3
"""Synthetic plugins only: no RFAB/game binaries or installed-profile assumptions."""
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

import generate_storage_profile as g


def field(kind: bytes, data: bytes) -> bytes:
    return kind + struct.pack("<H", len(data)) + data


def record(kind: bytes, raw: int, data: bytes = b"", flags: int = 0) -> bytes:
    return kind + struct.pack("<III", len(data), flags, raw) + bytes(8) + data


def text(value: str) -> bytes:
    b = value.encode()
    return struct.pack("<H", len(b)) + b


def script_object(raw: int, fmt: int = 2, alias: int = -1) -> bytes:
    return struct.pack("<HhI", 0, alias, raw) if fmt == 2 else struct.pack("<IhH", raw, alias, 0)


def vmad(raw: int, fmt: int = 2, alias: int = -1, count: int = 1) -> bytes:
    prop = text("SecureChest") + bytes([1, 1]) + script_object(raw, fmt, alias)
    return struct.pack("<HHH", 5, fmt, 1) + text("RFAB_SecureChestScript") + bytes([0]) + struct.pack("<H", count) + prop * count


def group(label: int, kind: int, body: bytes) -> bytes:
    return b"GRUP" + struct.pack("<IIi", len(body) + 24, label, kind) + bytes(8) + body


def ref(raw: int, base: int, script: bytes = b"", flags: int = 0, *, cell: int | None = 0x165A8,
        world: int | None = None, xyz=(100., 200., 0.), with_position=True) -> bytes:
    data = field(b"NAME", struct.pack("<I", base))
    if script:
        data += field(b"VMAD", script)
    if with_position:
        data += field(b"DATA", struct.pack("<6f", *xyz, 0., 0., 0.))
    result = record(b"REFR", raw, data, flags)
    if cell:
        result = group(cell, 6, group(cell, 9, result))
    return group(world, 1, result) if world else result


def plugin(body: bytes, masters: tuple[str, ...] = (), light: bool = False) -> bytes:
    header = b"".join(field(b"MAST", master.encode() + b"\0") + field(b"DATA", bytes(8)) for master in masters)
    return record(b"TES4", 0, header, 0x200 if light else 0) + body


class StorageProfileTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.base = self.root / "Skyrim.esm"
        self.base.write_bytes(plugin(record(b"CONT", 0x100) + record(b"ACTI", 0x101) +
                                     record(b"CELL", 0x165A8, field(b"DATA", struct.pack("<H", 1)))))
        self.rfab = self.root / "RFAB.esp"

    def tearDown(self):
        self.tmp.cleanup()

    def write_rfab(self, extra=b"", fmt=2, alias=-1):
        # Base forms are from master0; this plugin's own forms use master index1.
        body = ref(0x01000800, 0x100)
        body += ref(0x01000801, 0x101, vmad(0x01000800, fmt, alias))
        body += ref(0x01000802, 0x101, vmad(0x01000800, fmt, alias))
        self.rfab.write_bytes(plugin(body + extra, ("Skyrim.esm",)))

    def test_distinct_cities_resolve_to_same_hidden_backing(self):
        self.write_rfab()
        tsv, manifest = g.generate([self.base, self.rfab])
        self.assertEqual(len(manifest["storage"]), 2)
        self.assertEqual(manifest["storage"][0]["backing"], manifest["storage"][1]["backing"])
        self.assertIn("rfab.esp\t000801\t0\trfab.esp\t000800\t0\tskyrim.esm\t0165a8\t0\t-\t0\t0\t100\t200\t0\n", tsv)
        self.assertNotIn(str(self.root), str(manifest))

    def test_missing_parent_or_position_is_not_replaced_by_client_coordinates(self):
        for kwargs in [{"cell": None}, {"with_position": False}, {"cell": 0x12345}, {"xyz": (float("nan"), 0., 0.)},
                       {"xyz": (float("inf"), 0., 0.)}, {"xyz": (100_000_008., 0., 0.)}]:
            self.write_rfab(ref(0x01000801, 0x101, vmad(0x01000800), **kwargs))
            with self.subTest(kwargs=kwargs), self.assertRaises(g.ProfileError):
                g.generate([self.base, self.rfab])

    def test_winning_ref_position_and_parent_override_are_in_the_manifest(self):
        self.write_rfab()
        patch = self.root / "Access Patch.esp"
        cell = record(b"CELL", 0x02000820, field(b"DATA", struct.pack("<H", 1)))
        patch.write_bytes(plugin(cell + ref(0x01000801, 0x101, vmad(0x01000800), cell=0x02000820,
                                          xyz=(-1234.25, 567.5, -32.)), ("Skyrim.esm", "RFAB.esp")))
        tsv, manifest = g.generate([self.base, self.rfab, patch])
        self.assertIn("access patch.esp\t000820\t0\t-\t0\t0\t-1234.25\t567.5\t-32\n", tsv)
        row = next(r for r in manifest["storage"] if r["access"]["local"] == "000801")
        self.assertEqual(row["cell"]["plugin"], "access patch.esp")
        self.assertEqual(row["position"], (-1234.25, 567.5, -32.))
        self.assertEqual(manifest["schema"], 2)

    def test_exterior_persistent_cell_keeps_world_and_exact_static_coordinates(self):
        base = self.base.read_bytes()
        exterior = group(0x3C, 1, record(b"CELL", 0x16000, field(b"DATA", struct.pack("<H", 0))))
        self.base.write_bytes(base + record(b"WRLD", 0x3C) + exterior)
        self.write_rfab(ref(0x01000801, 0x101, vmad(0x01000800), cell=0x16000, world=0x3C, xyz=(8193., -4097., 16.)))
        tsv, manifest = g.generate([self.base, self.rfab])
        self.assertIn("skyrim.esm\t016000\t0\tskyrim.esm\t00003c\t0\t8193\t-4097\t16\n", tsv)
        row = next(r for r in manifest["storage"] if r["access"]["local"] == "000801")
        self.assertEqual(row["world"]["local"], "00003c")
        # An interior CELL merely nested below a claimed world is contradictory, not fallback.
        self.write_rfab(ref(0x01000801, 0x101, vmad(0x01000800), world=0x3C))
        with self.assertRaisesRegex(g.ProfileError, "interior/exterior"):
            g.generate([self.base, self.rfab])

    def test_group_context_cannot_leak_or_conflict_between_refs(self):
        self.write_rfab(ref(0x01000801, 0x101, vmad(0x01000800), cell=None))
        with self.assertRaisesRegex(g.ProfileError, "parent CELL"):
            g.generate([self.base, self.rfab])
        bad = group(0x165A8, 6, group(0x12345, 9, ref(0x01000801, 0x101, vmad(0x01000800), cell=None)))
        self.write_rfab(bad)
        with self.assertRaisesRegex(g.ProfileError, "subgroup"):
            g.generate([self.base, self.rfab])

    def test_cell_flags_accept_both_documented_one_and_two_byte_layouts(self):
        self.write_rfab()
        patch = self.root / "Cell Flags.esp"
        for flags in [bytes([1]), bytes([1, 0])]:
            patch.write_bytes(plugin(record(b"CELL", 0x165A8, field(b"DATA", flags)), ("Skyrim.esm", "RFAB.esp")))
            with self.subTest(flags=flags):
                self.assertEqual(len(g.generate([self.base, self.rfab, patch])[1]["storage"]), 2)

    def test_deleted_or_ambiguous_parent_is_refused(self):
        self.write_rfab()
        patch = self.root / "Parent Patch.esp"
        for data in [record(b"CELL", 0x165A8, flags=0x20), record(b"CELL", 0x165A8),
                     record(b"CELL", 0x165A8, field(b"DATA", bytes(3)))]:
            patch.write_bytes(plugin(data, ("Skyrim.esm", "RFAB.esp")))
            with self.subTest(data=data), self.assertRaises(g.ProfileError):
                g.generate([self.base, self.rfab, patch])

    def test_format_one_has_trailing_alias_and_padding(self):
        self.write_rfab(fmt=1)
        self.assertEqual(len(g.generate([self.base, self.rfab])[1]["storage"]), 2)

    def test_inherited_base_property_and_reference_override(self):
        base = record(b"CONT", 0x01000810, field(b"VMAD", vmad(0x01000800)))
        self.write_rfab(base + ref(0x01000811, 0x01000810))
        _, manifest = g.generate([self.base, self.rfab])
        self.assertEqual(len(manifest["storage"]), 3)
        patch = self.root / "Bizarre Adventure.esp"
        # Override the first access point to use a distinct, real backing container.
        patch.write_bytes(plugin(ref(0x02000800, 0x100) + ref(0x01000801, 0x101, vmad(0x02000800)), ("Skyrim.esm", "RFAB.esp")))
        tsv, manifest = g.generate([self.base, self.rfab, patch])
        self.assertIn("rfab.esp\t000801\t0\tbizarre adventure.esp\t000800\t0", tsv)
        self.assertEqual(len(manifest["storage"]), 3)

    def test_winning_deleted_access_is_not_reanimated(self):
        self.write_rfab(record(b"REFR", 0x01000801, flags=0x20))
        self.assertEqual(len(g.generate([self.base, self.rfab])[1]["storage"]), 1)

    def test_alias_is_not_a_stable_reference(self):
        self.write_rfab(alias=7)
        with self.assertRaisesRegex(g.ProfileError, "alias"):
            g.generate([self.base, self.rfab])

    def test_null_target_wrong_type_and_duplicate_property_refused(self):
        for target, n in [(0, 1), (0x101, 1), (0x01000800, 2)]:
            self.write_rfab(ref(0x01000801, 0x101, vmad(target, count=n)))
            with self.subTest(target=target, count=n), self.assertRaises(g.ProfileError):
                g.generate([self.base, self.rfab])

    def test_esl_kind_from_header_not_filename_or_current_load_index(self):
        self.write_rfab()
        light = self.root / "Storage Patch.esp"
        light.write_bytes(plugin(ref(0x02000800, 0x100) + ref(0x01000801, 0x101, vmad(0x02000800)), ("Skyrim.esm", "RFAB.esp"), light=True))
        tsv, _ = g.generate([self.base, self.rfab, light])
        self.assertIn("storage patch.esp\t000800\t1", tsv)
        light.write_bytes(plugin(ref(0x02001800, 0x100), ("Skyrim.esm", "RFAB.esp"), light=True))
        with self.assertRaises(g.ProfileError):
            g.generate([self.base, self.rfab, light])

    def test_compressed_and_extended_records_are_bounded(self):
        self.write_rfab()
        payload = field(b"NAME", struct.pack("<I", 0x101)) + field(b"VMAD", vmad(0x01000800)) + field(b"DATA", struct.pack("<6f", 100., 200., 0., 0., 0., 0.))
        compressed = struct.pack("<I", len(payload)) + zlib.compress(payload)
        self.write_rfab(group(0x165A8, 6, group(0x165A8, 9, record(b"REFR", 0x01000803, compressed, 0x40000))))
        self.assertEqual(len(g.generate([self.base, self.rfab])[1]["storage"]), 3)
        # The extended length belongs to exactly the immediately following field.
        extended = b"XXXX" + struct.pack("<HI", 4, 4) + b"NAME" + bytes(2) + struct.pack("<I", 0x101)
        self.assertEqual(list(g.subrecords(extended)), [(b"NAME", struct.pack("<I", 0x101))])
        with self.assertRaises(g.ProfileError):
            list(g.records(record(b"REFR", 1, struct.pack("<I", g.MAX_RECORD + 1) + zlib.compress(b"x"), 0x40000)))

    def test_all_truncations_of_known_vmad_refused(self):
        self.write_rfab()
        ordered, plugins = g.load_plugins([self.base, self.rfab])
        data = vmad(0x01000800)
        for size in range(len(data)):
            with self.subTest(size=size), self.assertRaises((g.ProfileError, UnicodeError)):
                g.vmad(data[:size], ordered[1], plugins)

    def test_record_group_subrecord_and_zip_trailing_garbage_refused(self):
        invalid = [b"a", b"GRUP" + struct.pack("<I", 0) + bytes(16),
                   record(b"REFR", 1, struct.pack("<I", 4) + zlib.compress(b"test") + b"extra", 0x40000)]
        for data in invalid:
            with self.subTest(data=data[:12]), self.assertRaises(g.ProfileError):
                list(g.records(data))
        with self.assertRaises(g.ProfileError):
            list(g.subrecords(b"XXXX" + struct.pack("<HI", 4, 5)))

    def test_master_order_case_duplicate_and_path_safety(self):
        self.write_rfab()
        with self.assertRaises(g.ProfileError):
            g.generate([self.rfab, self.base])
        with self.assertRaises(g.ProfileError):
            g.generate([self.base, self.base])
        for name in ["../RFAB.esp", "C:\\RFAB.esp", "rfab.esp\n", " rfab.esp", "x|y.esp"]:
            with self.subTest(name=name), self.assertRaises(g.ProfileError):
                g.plugin_name(name)
        self.assertEqual(g.plugin_name("Bizarre Adventure.ESP"), "bizarre adventure.esp")

    def test_empty_profile_does_not_emit_an_empty_authoritative_allowlist(self):
        with self.assertRaisesRegex(g.ProfileError, "No verified"):
            g.generate([self.base])


if __name__ == "__main__":
    unittest.main()
