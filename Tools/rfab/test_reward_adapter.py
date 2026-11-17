#!/usr/bin/env python3
"""Synthetic installed-source/preimage/staging regressions, NOT an installed RFAB/game test."""
import copy
import hashlib
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import generate_reward_adapter as adapter


HANDLER = '''Scriptname SyntheticXP extends Quest
; Синтетический пример, НЕ исходник установленного RFAB.
GlobalVariable Property Wallet Auto
GlobalVariable Property Multiplier Auto
Actor Property PlayerRef Auto
int Infamy = 0
bool CanNotify = true
Function KillReward(Actor victim, int amount)
    Infamy -= 1
    int xp = CalculateXP(amount)
    ModXP(xp)
    Infamy += 2
EndFunction
Function BookReward(Book book, int amount)
    int xp = CalculateXP(amount)
    ModXP(xp)
EndFunction
Function QuestReward(Quest sourceQuest, int amount, string occurrence)
    int xp = CalculateXP(amount)
    ModXP(xp)
EndFunction
Function CustomReward(int amount)
    int xp = CalculateXP(amount)
    ModXP(xp)
EndFunction
Function Spend(int cost)
    ModXP(-cost)
EndFunction
Function ModXP(int xp)
    Wallet.Mod(xp)
    Game.SetPlayerExperience(Game.GetPlayerExperience() + xp)
    if Game.GetPlayerExperience() >= Game.GetExperienceForLevel(PlayerRef.GetLevel()) && CanNotify
        CanNotify = false
        NotifyLevel()
    endif
EndFunction
int Function CalculateXP(int baseXP)
    int personalXP = (baseXP * Multiplier.GetValue()) as int
    return personalXP - (personalXP % 5)
EndFunction
Function NotifyLevel()
    Debug.Notification("Level ready; not a second XP award")
EndFunction
'''
ALIAS = '''Scriptname SyntheticAlias extends ReferenceAlias
SyntheticXP Property SystemXP Auto
Event OnPlayerLoadGame()
EndEvent
Event OnInit()
EndEvent
Function RewardCustom(Form sender, string slot, int amount)
    SystemXP.CustomReward(amount)
EndFunction
'''


def sha(data):
    return hashlib.sha256(data).hexdigest()


def fixture(root):
    handler = HANDLER.replace('\n', '\r\n').encode('cp1251')
    alias = ALIAS.encode()
    (root / 'SyntheticXP.psc').write_bytes(handler)
    (root / 'SyntheticAlias.psc').write_bytes(alias)
    hooks = []
    for kind, method, source, actor in ((0, 'KillReward', 'victim', 'victim'), (1, 'QuestReward', 'sourceQuest', None),
                                         (2, 'BookReward', 'book', None), (3, 'CustomReward', 'sender', None)):
        hook = dict(script='SyntheticXP.psc', method=method, first='int xp = CalculateXP(amount)', last='ModXP(xp)',
                    kind=kind, source=source, context=None, actor=actor, life=adapter.ZERO,
                    occurrence=adapter.ONE if kind in (0, 2) else {'parameter': 'occurrence' if kind == 1 else 'slot'})
        if kind in (1, 3):
            hook['identity_policy'] = 'explicit-caller'
        hooks.append(hook)
    hooks[3]['forward'] = dict(name='RFABCoopCustomReward', parameters=[['Form', 'sender', 'None'], ['string', 'slot', '"' + adapter.ZERO + '"']],
                               callsites=[dict(script='SyntheticAlias.psc', method='RewardCustom',
                                               before='SystemXP.CustomReward(amount)',
                                               after='SystemXP.RFABCoopCustomReward(sender, slot, amount)')])
    return dict(schema=1, name='synthetic-review-only', bizarre=False,
                files={'SyntheticXP.psc': dict(sha256=sha(handler), encoding='cp1251'),
                       'SyntheticAlias.psc': dict(sha256=sha(alias), encoding='utf-8')},
                native_files={},
                receiver=dict(script='SyntheticXP.psc', wallet='Wallet', multiplier='Multiplier', quantum=5,
                              engine_pool='skse-level-points', wallet_pool='global-variable-setter',
                              level_notification=dict(player='PlayerRef', flag='CanNotify', function='NotifyLevel')),
                hooks=hooks, personal_spending_methods=[dict(script='SyntheticXP.psc', method='Spend')],
                registrations=[dict(script='SyntheticAlias.psc', method='OnPlayerLoadGame', receiver='SystemXP'),
                               dict(script='SyntheticAlias.psc', method='OnInit', receiver='SystemXP')])


class RewardAdapterTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.profile = fixture(self.root)

    def prepare(self, profile=None):
        return adapter.prepare(profile or self.profile, self.root)

    def replace_source(self, name, before, after):
        spec = self.profile['files'][name]
        path = self.root / name
        data = path.read_bytes().decode(spec['encoding'])
        self.assertIn(before, data)
        data = data.replace(before, after).encode(spec['encoding'])
        path.write_bytes(data)
        spec['sha256'] = sha(data)

    def test_actual_pre_multiplier_capture_and_distinct_semantic_sources(self):
        output, manifest = self.prepare()
        script = output['SyntheticXP.psc'].decode('cp1251')
        self.assertEqual(manifest['kinds'], 15)
        self.assertFalse(manifest['compiled'])
        self.assertFalse(manifest['game_validated'])
        self.assertEqual(script.count('RFABCoopRewards.Submit('), 4)
        self.assertEqual(script.count('int RFABCoopBase = amount'), 4)
        self.assertEqual(script.count('CalculateXP(RFABCoopBase)'), 4)
        self.assertIn('victim.GetFormID()', script)
        self.assertIn('sourceQuest.GetFormID()', script)
        self.assertIn('book.GetFormID()', script)
        self.assertIn('sender.GetFormID()', script)
        self.assertIn('RFABCoopRewards.RequestPersonalApply(ticket, Wallet.GetFormID(), Multiplier.GetFormID(), 5)', script)
        # Source report != application. No caller-supplied final/base on the application native.
        self.assertNotIn('RequestPersonalApply(ticket, baseXP', script)
        self.assertNotIn('RFABCoopRewards.Complete(', script)
        self.assertNotIn('RFABCoopRewards.Claim(', script)

    def test_preserves_original_bytes_encoding_line_endings_and_non_xp_side_effects(self):
        original = (self.root / 'SyntheticXP.psc').read_bytes()
        output, _ = self.prepare()
        script = output['SyntheticXP.psc'].decode('cp1251')
        self.assertIn('; Синтетический', script)
        self.assertNotIn('\n', script.replace('\r\n', ''))
        self.assertEqual((self.root / 'SyntheticXP.psc').read_bytes(), original)
        self.assertIn('    Infamy -= 1\r\n    int RFABCoopBase', script)
        self.assertIn('    endif\r\n    Infamy += 2', script)
        self.assertIn('Function Spend(int cost)\r\n    ModXP(-cost)\r\nEndFunction', script)
        self.assertIn('    if Game.GetPlayerExperience() >= Game.GetExperienceForLevel(PlayerRef.GetLevel()) && CanNotify', script)

    def test_identity_losing_old_entry_is_not_guessed_online_and_real_callsite_is_forwarded(self):
        output, _ = self.prepare()
        script = output['SyntheticXP.psc'].decode('cp1251')
        self.assertIn('Function CustomReward(int amount)\r\n    RFABCoopCustomReward(None, "' + adapter.ZERO + '", amount)', script)
        self.assertIn('Function RFABCoopCustomReward(Form sender, string slot, int amount)', script)
        self.assertIn('RFABCoopBase, "' + adapter.ZERO + '", slot) == 0', script)
        alias = output['SyntheticAlias.psc'].decode()
        self.assertIn('SystemXP.RFABCoopCustomReward(sender, slot, amount)', alias)
        self.assertEqual(alias.count('SystemXP.RFABCoopRegister()'), 2)

    def test_exact_preimage_failure_has_no_output_and_never_force_patches_an_update(self):
        output = self.root / 'new-mod'
        (self.root / 'SyntheticXP.psc').write_bytes(b'new installed build')
        with self.assertRaisesRegex(adapter.InvalidProfile, 'preimage mismatch'):
            adapter.build(self.profile, self.root, output)
        self.assertFalse(output.exists())

    def test_unknown_formula_even_with_new_sha_is_refused(self):
        for expression in ('RFAB.GetPersonalXP(baseXP)', '(baseXP * Multiplier.GetValue()) as int + 10'):
            with self.subTest(expression=expression):
                path = self.root / 'SyntheticXP.psc'
                data = HANDLER.replace('(baseXP * Multiplier.GetValue()) as int', expression).encode('cp1251')
                path.write_bytes(data)
                self.profile['files']['SyntheticXP.psc']['sha256'] = sha(data)
                with self.assertRaisesRegex(adapter.InvalidProfile, 'formula'):
                    self.prepare()

    def test_unknown_native_pool_write_or_extra_side_effect_is_not_skipped(self):
        for before, after in (('Wallet.Mod(xp)', 'RFAB.ModXP(xp)'),
                              ('Game.SetPlayerExperience(Game.GetPlayerExperience() + xp)', 'RFAB.ApplyLevelPoints(xp)'),
                              ('    Wallet.Mod(xp)', '    Wallet.Mod(xp)\r\n    Infamy += 1')):
            self.profile = fixture(self.root)
            self.replace_source('SyntheticXP.psc', before, after)
            with self.assertRaises(adapter.InvalidProfile):
                self.prepare()

    def test_unknown_engine_pool_and_missing_attached_property_are_refused(self):
        for key, value in (('engine_pool', 'AddSkillExperience'), ('wallet_pool', 'raw-memory'), ('multiplier', 'GuessedRFABGlobal')):
            with self.subTest(key=key):
                profile = copy.deepcopy(self.profile)
                profile['receiver'][key] = value
                with self.assertRaises(adapter.InvalidProfile):
                    self.prepare(profile)

    def test_repeatable_custom_identity_cannot_be_time_wallet_text_or_a_constant(self):
        for occurrence in (adapter.ONE, 'quest complete', {'parameter': 'GetRealHoursPassed()'}, {'parameter': 'amount'}):
            with self.subTest(occurrence=occurrence):
                profile = copy.deepcopy(self.profile)
                profile['hooks'][1]['occurrence'] = occurrence
                with self.assertRaises(adapter.InvalidProfile):
                    self.prepare(profile)

    def test_book_needs_real_form_and_kill_needs_the_victim_reference(self):
        for index, changes in ((2, {'source': None}), (2, {'life': adapter.ONE}), (0, {'actor': 'BaseId'})):
            profile = copy.deepcopy(self.profile)
            profile['hooks'][index].update(changes)
            with self.assertRaises(adapter.InvalidProfile):
                self.prepare(profile)

    def test_new_uncovered_modxp_call_or_overlapping_regions_cannot_slip_through(self):
        self.replace_source('SyntheticXP.psc', '    Infamy += 2', '    Infamy += 2\r\n    ModXP(100)')
        with self.assertRaisesRegex(adapter.InvalidProfile, 'uncovered ModXP'):
            self.prepare()
        self.profile = fixture(self.root)
        self.profile['hooks'].append(copy.deepcopy(self.profile['hooks'][0]))
        with self.assertRaisesRegex(adapter.InvalidProfile, 'overlapping'):
            self.prepare()

    def test_a_reward_region_cannot_cross_control_flow_or_return_out_of_its_method(self):
        for body in ('    if true\r\n    ModXP(xp)', '    return\r\n    ModXP(xp)'):
            self.profile = fixture(self.root)
            self.replace_source('SyntheticXP.psc', '    ModXP(xp)', body)
            with self.assertRaises(adapter.InvalidProfile):
                self.prepare()

    def test_changed_or_ambiguous_callsite_is_refused(self):
        for statement in ('SystemXP.CustomReward(amount + 1)', 'SystemXP.CustomReward(amount)\n    SystemXP.CustomReward(amount)'):
            self.profile = fixture(self.root)
            self.replace_source('SyntheticAlias.psc', 'SystemXP.CustomReward(amount)', statement)
            with self.assertRaisesRegex(adapter.InvalidProfile, 'preimage line'):
                self.prepare()

    def test_traversal_symlinks_double_patch_and_unreviewed_states_are_refused(self):
        for name in ('../SyntheticXP.psc', '/tmp/Test.psc', 'folder/Script.psc', 'X' * 65 + '.psc'):
            with self.subTest(name=name), self.assertRaises(adapter.InvalidProfile):
                adapter.safe_file(name)
        (self.root / 'real.psc').write_bytes((self.root / 'SyntheticXP.psc').read_bytes())
        (self.root / 'SyntheticXP.psc').unlink()
        (self.root / 'SyntheticXP.psc').symlink_to(self.root / 'real.psc')
        with self.assertRaises(adapter.InvalidProfile):
            self.prepare()
        (self.root / 'SyntheticXP.psc').unlink()
        for suffix in ('\n; RFABCoop already patched\n', '\nauto state Rewarding\nendstate\n'):
            self.profile = fixture(self.root)
            self.replace_source('SyntheticXP.psc', 'Scriptname SyntheticXP extends Quest', 'Scriptname SyntheticXP extends Quest' + suffix)
            with self.assertRaises(adapter.InvalidProfile):
                self.prepare()

    def test_native_dll_is_an_exact_preimage_not_an_inferred_bizarre_flag(self):
        dll = self.root / 'SKSE/Plugins/RFAB.dll'
        dll.parent.mkdir(parents=True)
        dll.write_bytes(b'synthetic native fingerprint only')
        self.profile['native_files'] = {'SKSE/Plugins/RFAB.dll': sha(dll.read_bytes())}
        with self.assertRaisesRegex(adapter.InvalidProfile, 'data-root'):
            self.prepare()
        _, manifest = adapter.prepare(self.profile, self.root, self.root)
        self.assertFalse(manifest['bizarre'])
        dll.write_bytes(b'newer native build')
        with self.assertRaisesRegex(adapter.InvalidProfile, 'preimage mismatch'):
            adapter.prepare(self.profile, self.root, self.root)

    def test_profile_signature_covers_every_exact_input_and_recipe_but_not_personal_wallet_values(self):
        output, first = self.prepare()
        self.assertEqual(self.prepare(), (output, first))
        reordered = dict(reversed(list(self.profile.items())))
        self.assertEqual(self.prepare(reordered)[1]['signature'], first['signature'])
        changed = copy.deepcopy(self.profile)
        changed['bizarre'] = True
        self.assertNotEqual(self.prepare(changed)[1]['signature'], first['signature'])
        self.assertEqual(set(first['input_sha256']), {'SyntheticXP.psc', 'SyntheticAlias.psc'})

    def test_source_only_is_explicitly_uncompiled_not_an_activatable_capability(self):
        output = self.root / 'review-output'
        manifest = adapter.build(self.profile, self.root, output)
        self.assertFalse(manifest['compiled'])
        self.assertTrue((output / 'UNCOMPILED-NOT-A-MOD.txt').is_file())
        self.assertFalse(list(output.rglob('*.pex')))
        self.assertFalse(list(output.rglob('*.tsv')))
        self.assertEqual(json.loads((output / 'RFAB-reward-profile.json').read_text()), manifest)
        before = (output / 'scripts/source/SyntheticXP.psc').read_bytes()
        with self.assertRaisesRegex(adapter.InvalidProfile, 'NEW directory'):
            adapter.build(self.profile, self.root, output)
        self.assertEqual((output / 'scripts/source/SyntheticXP.psc').read_bytes(), before)

    def test_mocked_compiler_failure_never_publishes_a_partial_mod(self):
        compiler, flags = self.root / 'compiler', self.root / 'flags'
        compiler.touch(); flags.touch()
        output = self.root / 'candidate'
        with patch.object(adapter.subprocess, 'run', return_value=SimpleNamespace(returncode=1, stdout='synthetic failure')):
            with self.assertRaisesRegex(adapter.InvalidProfile, 'compilation failed'):
                adapter.build(self.profile, self.root, output, compiler=compiler, flags=flags, imports=(self.root,))
        self.assertFalse(output.exists())

    def test_mocked_compiler_output_hashes_all_compiled_scripts_and_api_for_runtime_recheck(self):
        compiler, flags = self.root / 'compiler', self.root / 'flags'
        compiler.touch(); flags.touch()
        output = self.root / 'compiled-candidate'

        def simulated_compile(command, **_):
            # Exercises staging/hash generation ONLY. These bytes are NOT executable Papyrus.
            dest = Path(next(x[3:] for x in command if x.startswith('-o='))) / (Path(command[1]).stem + '.pex')
            dest.write_bytes(bytes.fromhex('fa57c0de03020001') + b'SYNTHETIC-NOT-EXECUTABLE')
            return SimpleNamespace(returncode=0, stdout='mocked compiler')

        with patch.object(adapter.subprocess, 'run', side_effect=simulated_compile):
            manifest = adapter.build(self.profile, self.root, output, compiler=compiler, flags=flags, imports=(self.root,))
        self.assertTrue(manifest['compiled'])
        self.assertFalse(manifest['game_validated'])
        table = (output / 'SKSE/Plugins/RFAB_Rewards.tsv').read_text().splitlines()
        self.assertEqual(table[0], 'RFAB_REWARDS_V1\tsynthetic-review-only\t' + manifest['signature'] + '\t15\t0\tpapyrus-float-v1')
        hashes = dict(line.split('\t')[::-1] for line in table[1:])
        self.assertEqual(set(hashes), {'scripts/SyntheticXP.pex', 'scripts/SyntheticAlias.pex', 'scripts/RFABCoopRewards.pex'})
        for relative, digest in hashes.items():
            self.assertEqual(sha((output / relative).read_bytes()), digest)


if __name__ == '__main__':
    unittest.main()
