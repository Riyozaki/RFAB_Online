#!/usr/bin/env python3
"""Prepare exact-preimage, reviewed installed-RFAB Papyrus XP overrides in a NEW directory.

No network, installation, PEX decompilation, guessed current RFAB or whole historical Handler
replacement. A profile must identify real semantic sources/cycles/slots and every edited input
by SHA256. Unknown source/formula/native DLL changes refuse before output. Source-only output
is explicitly UNCOMPILED; a real CK Papyrus compiler + installed imports is optional.
"""
from __future__ import annotations
import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
IDENT = r"[A-Za-z_][A-Za-z_0-9]{0,63}"
HEX = re.compile(r"[0-9a-f]{16}")
SHA = re.compile(r"[0-9a-f]{64}")
ZERO, ONE = "0000000000000000", "0000000000000001"
VERSION = 1


class InvalidProfile(ValueError):
    pass


def require(condition, reason):
    if not condition:
        raise InvalidProfile(reason)


def identifier(value):
    require(isinstance(value, str) and re.fullmatch(IDENT, value), "expected a bounded Papyrus identifier")
    return value


def code(line):
    """Strip comments only outside quoted strings; never turn a string into a call site."""
    quoted, escaped, result = False, False, []
    for char in line:
        if char == ";" and not quoted:
            break
        result.append(char)
        if char == '"' and not escaped:
            quoted = not quoted
        escaped = char == "\\" and not escaped
    require(not quoted, "multi-line/unterminated Papyrus string is unsupported")
    return "".join(result).strip()


@dataclass
class Method:
    start: int
    end: int
    kind: str
    result: str
    name: str
    parameters: str

    def args(self):
        result = []
        for item in self.parameters.split(",") if self.parameters.strip() else []:
            match = re.fullmatch(rf"\s*({IDENT})\s+({IDENT})(?:\s*=\s*[^,]+)?\s*", item)
            require(match, "unsupported method parameter syntax")
            result.append((match[1], match[2]))
        return result


class Source:
    def __init__(self, name, data, encoding):
        require(len(data) <= 1024 * 1024, f"{name}: script exceeds 1 MiB")
        require(encoding in ("utf-8", "utf-8-sig", "cp1251"), "source encoding must be explicit UTF-8 or CP1251")
        try:
            text = data.decode(encoding)
        except UnicodeError as error:
            raise InvalidProfile(f"{name}: source encoding mismatch") from error
        require("RFABCoop" not in text, f"{name}: already adapted/reserved RFABCoop identifier; never patch twice")
        require("\x00" not in text and ";/" not in text, "NUL/block-comment sources need a reviewed parser recipe")
        self.name, self.encoding = name, encoding
        self.newline = "\r\n" if "\r\n" in text else "\n"
        self.trailing = text.endswith("\n")
        self.lines = text.splitlines()
        declarations = [code(s) for s in self.lines if code(s)]
        require(declarations and re.match(rf"Scriptname\s+{re.escape(Path(name).stem)}(?:\s|$)", declarations[0], re.I),
                f"{name}: Scriptname differs from filename")
        require(not any(re.match(r"(?:auto\s+)?state(?:\s|$)", s, re.I) for s in declarations),
                f"{name}: state-specific dispatch needs its own recipe")
        self.methods()  # validate complete, unambiguous methods before any edits

    def methods(self):
        result, active = {}, None
        header = re.compile(rf"(?:(?P<result>{IDENT})\s+)?(?P<kind>Function|Event)\s+(?P<name>{IDENT})\s*\((?P<params>[^()]*)\)\s*(?P<flags>.*)$", re.I)
        for index, line in enumerate(self.lines):
            text = code(line)
            match = header.fullmatch(text)
            if match:
                require(active is None, f"{self.name}: nested method or unsupported multiline header")
                if re.search(r"\bnative\b", match['flags'], re.I):
                    continue
                require(not match['flags'].strip(), f"{self.name}: only instance functions/events can be adapted")
                active = Method(index, -1, match['kind'], match['result'] or "", match['name'], match['params'])
                require(active.name.casefold() not in result, f"{self.name}: ambiguous/duplicate method")
            elif re.fullmatch(r"EndFunction|EndEvent", text, re.I):
                require(active and text.casefold() == "end" + active.kind.casefold(), f"{self.name}: unpaired method end")
                active.end = index
                active.args()
                result[active.name.casefold()] = active
                active = None
        require(active is None, f"{self.name}: unterminated method")
        return result

    def method(self, name):
        found = self.methods().get(identifier(name).casefold())
        require(found is not None, f"{self.name}: missing method {name}")
        return found

    def unique_line(self, method, expected):
        require(isinstance(expected, str) and expected == code(expected), "profile needs an exact non-comment logical line")
        matches = [i for i in range(method.start + 1, method.end) if code(self.lines[i]) == expected]
        require(len(matches) == 1, f"{self.name}::{method.name}: missing/ambiguous preimage line: {expected}")
        return matches[0]

    def bytes(self):
        return (self.newline.join(self.lines) + (self.newline if self.trailing else "")).encode(self.encoding)


def safe_file(name):
    require(isinstance(name, str) and re.fullmatch(rf"{IDENT}\.psc", name), "only flat, bounded .psc filenames are allowed")
    return name


def read_exact(root, relative, expected):
    require(isinstance(expected, str) and SHA.fullmatch(expected), "every edited source/native input needs its exact SHA256")
    relative = Path(relative)
    require(not relative.is_absolute() and not any(p in ("..", ".") for p in relative.parts), "input path escapes its root")
    path = root / relative
    require(path.is_file() and not path.is_symlink() and root.resolve() in path.resolve().parents, f"missing/unsafe input: {relative}")
    require(path.stat().st_size <= 128 * 1024 * 1024, f"input exceeds 128 MiB: {relative}")
    data = path.read_bytes()
    require(hashlib.sha256(data).hexdigest() == expected, f"preimage mismatch: {relative}; do not force an old recipe onto this installed build")
    return data


def formula(source, receiver):
    wallet, multiplier = identifier(receiver['wallet']), identifier(receiver['multiplier'])
    require(wallet.casefold() != multiplier.casefold(), "wallet and multiplier must be distinct properties")
    for prop in (wallet, multiplier):
        pattern = rf"GlobalVariable\s+Property\s+{re.escape(prop)}\s+Auto(?:ReadOnly)?"
        require(sum(bool(re.fullmatch(pattern, code(s), re.I)) for s in source.lines) == 1,
                f"{source.name}: expected attached GlobalVariable property {prop}")
    quantum = receiver['quantum']
    require(type(quantum) is int and 1 <= quantum <= 1000, "invalid rounding quantum")
    method = source.method(receiver.get('calculate', 'CalculateXP'))
    require(method.result.casefold() == "int" and len(method.args()) == 1 and method.args()[0][0].casefold() == "int",
            "supported CalculateXP recipe takes/returns one int")
    parameter = method.args()[0][1]
    body = [re.sub(r"\s+", "", code(s)).casefold() for s in source.lines[method.start + 1:method.end] if code(s)]
    require(len(body) == 2, "unknown/native/extra personal multiplier formula; supply a different adapter")
    first = re.fullmatch(rf"int({IDENT})=\({re.escape(parameter.casefold())}\*{re.escape(multiplier.casefold())}\.getvalue\(\)\)asint", body[0])
    require(first and body[1] == f"return{first[1]}-({first[1]}%{quantum})",
            "formula is not float multiply -> int truncation -> declared quantum; no guessed RFAB.dll substitute")
    require(receiver.get('engine_pool') == 'skse-level-points' and receiver.get('wallet_pool') == 'global-variable-setter',
            "this adapter cannot substitute for unknown extra/native XP pools")
    audit_pools(source, receiver, wallet)
    return wallet, multiplier, quantum


def audit_pools(source, receiver, wallet):
    """This built-in executor may replace ONLY the two explicitly audited native stores.
    An updated RFAB.dll, statistics/bonus write or different rounding is not silently skipped.
    """
    def body(method):
        require(not method.result and len(method.args()) == 1 and method.args()[0][0].casefold() == 'int',
                'personal pool writer must be a void instance function with one int parameter')
        return [re.sub(r'\s+', '', code(line)).casefold() for line in source.lines[method.start + 1:method.end] if code(line)]

    mod = source.method(receiver.get('modxp', 'ModXP'))
    lines = body(mod)
    param = mod.args()[0][1].casefold()
    require(len(lines) >= 2 and lines[0] == wallet.casefold() + '.mod(' + param + ')',
            'ModXP does not use exactly the attached wallet GlobalVariable.Mod path')
    writer_name = receiver.get('engine_writer')
    tail = lines[2:]
    if writer_name:
        writer = source.method(identifier(writer_name))
        engine = body(writer)
        engine_param = writer.args()[0][1].casefold()
        require(lines[1] == writer_name.casefold() + '(' + param + ')' and engine and
                engine[0] == 'game.setplayerexperience(game.getplayerexperience()+' + engine_param + ')',
                'unknown native/extra engine pool path; no substituted RFAB.dll formula')
        require(not (tail and engine[1:]), 'ambiguous duplicated personal pool notifications')
        tail += engine[1:]
    else:
        require(lines[1] == 'game.setplayerexperience(game.getplayerexperience()+' + param + ')',
                'unknown native/extra engine pool path; no substituted RFAB.dll formula')
    notice = receiver.get('level_notification')
    expected = []
    if notice:
        player, flag, function = (identifier(notice[k]).casefold() for k in ('player', 'flag', 'function'))
        expected = ['ifgame.getplayerexperience()>=game.getexperienceforlevel(' + player + '.getlevel())&&' + flag,
                    flag + '=false', function + '()', 'endif']
        notify = source.method(notice['function'])
        require(not notify.result and not notify.args(), 'level notification must be void and take no arguments')
    require(tail == expected, 'unknown personal pool side effects/notification: needs another explicitly audited adapter')


def forward(source, hook, sources):
    """Retain an old no-identity entry as a blocked-online/offline-compatible forwarder.
    Only explicitly reviewed caller lines gain the real Form/cycle/slot arguments.
    """
    item = hook.get('forward')
    if not item:
        return hook['method']
    old = source.method(hook['method'])
    require(old.kind.casefold() == 'function', 'events cannot be changed into forwarding functions')
    name = identifier(item['name'])
    require(name.startswith('RFABCoop') and name.casefold() not in source.methods(), "invalid/colliding forwarding method")
    parameters, defaults = [], []
    for kind, param, default in item['parameters']:
        require(kind.casefold() in ('form', 'book', 'actor', 'quest', 'string', 'int'), "unsupported identity parameter type")
        identifier(param)
        require(default in ('None', '0', '"' + ZERO + '"'), "old unidentified entry must have an invalid/zero identity default")
        parameters.append(kind + ' ' + param)
        defaults.append(default)
    require(parameters and len(parameters) <= 4 and item.get('callsites'), "forwarding needs 1..4 identity parameters and reviewed callers")
    arguments = defaults + [name for _, name in old.args()]
    clone_params = ', '.join(parameters + ([old.parameters] if old.parameters.strip() else []))
    body = source.lines[old.start + 1:old.end]
    prefix = (old.result + ' ') if old.result else ''
    call = name + '(' + ', '.join(arguments) + ')'
    replacement = [source.lines[old.start], '    ' + ('return ' if old.result else '') + call, source.lines[old.end], '',
                   prefix + 'Function ' + name + '(' + clone_params + ')', *body, 'EndFunction']
    source.lines[old.start:old.end + 1] = replacement
    for caller in item['callsites']:
        target = sources[safe_file(caller['script'])]
        method = target.method(caller['method'])
        before, after = caller['before'], caller['after']
        require(isinstance(after, str) and '\n' not in after and '\r' not in after and code(after) == after,
                "caller replacement must be one reviewed logical line")
        require(name + '(' in after and not re.search(r"GetCurrentRealTime|GetRealHoursPassed|GetPlayerExperience|CalculateXP|GetValue\(", after, re.I),
                "caller must pass real semantic identities, never time/amount/wallet-derived tokens")
        line = target.unique_line(method, before)
        indent = re.match(r"\s*", target.lines[line])[0]
        target.lines[line] = indent + after
    return name


def identity(value, method, label):
    if isinstance(value, str) and HEX.fullmatch(value):
        return '"' + value + '"'
    require(isinstance(value, dict) and set(value) == {'parameter'}, f"{label}: expected fixed hex or explicit caller string parameter")
    param = identifier(value['parameter'])
    require(('string', param.casefold()) in [(t.casefold(), n.casefold()) for t, n in method.args()],
            f"{label}: identity must be a method's real string parameter, not a guessed expression")
    return param


def gate(source, method_name, hook, receiver, receiver_script):
    method = source.method(method_name)
    require(not any('RFABCoopBase' in line for line in source.lines[method.start + 1:method.end]),
            'multiple award spans in one method need a separate reviewed recipe; no duplicate Papyrus locals')
    start, end = source.unique_line(method, hook['first']), source.unique_line(method, hook['last'])
    require(start <= end, "reversed award region")
    region = source.lines[start:end + 1]
    balance = []
    for line in region:
        text = code(line).casefold()
        if re.match(r"(?:if|while)\b", text):
            balance.append(text.split()[0])
        if re.fullmatch(r"endif|endwhile", text):
            require(balance and 'end' + balance.pop() == text, "reward region crosses an enclosing control-flow boundary")
        require(not re.match(r"return\b", text), "reward region return would skip unrelated side effects; needs another recipe")
    require(not balance, "reward region is not a complete control-flow span")
    # Capture the actual pre-multiplier expression ONCE; don't call getters/calculators twice.
    calculate = identifier(receiver.get('calculate', 'CalculateXP'))
    require(hook.get('calculate', calculate).casefold() == calculate.casefold(), 'hook uses a different personal formula')
    first = code(region[0])
    match = re.fullmatch(rf"(?P<prefix>(?:int\s+)?{IDENT}\s*=\s*(?:(?P<owner>{IDENT})\.)?{calculate}\()(?P<base>.+)\)", first, re.I)
    require(match, "award region must begin at its audited pre-personal-multiplier CalculateXP call")
    if source.name.casefold() == receiver_script.casefold():
        require(not match['owner'] or match['owner'].casefold() == 'self', 'a different calculator needs its own receiver/formula audit')
    else:
        owner = identifier(match['owner'])
        expected = rf"{re.escape(Path(receiver_script).stem)}\s+Property\s+{re.escape(owner)}\s+Auto"
        require(sum(bool(re.fullmatch(expected, code(line), re.I)) for line in source.lines) == 1,
                'external CalculateXP must use the actual typed attached receiver property')
    require(sum(len(re.findall(r"\bModXP\s*\(", code(s), re.I)) for s in region) == 1,
            "award region must contain exactly one original ModXP call")
    kind = hook['kind']
    require(type(kind) is int and 0 <= kind <= 3, "invalid semantic kind")
    life_value, occurrence_value = hook['life'], hook['occurrence']
    if kind in (0, 2):
        require(life_value == ZERO and occurrence_value == ONE and hook.get('context') is None,
                "kill/book use their explicit canonical life/occurrence policy")
    else:
        policy = hook.get('identity_policy')
        require(policy in ('once-per-campaign', 'explicit-caller'), "quest/custom repeat policy must be explicitly reviewed")
        if policy == 'explicit-caller':
            require(isinstance(occurrence_value, dict), "repeatable/custom slot identity cannot be inferred from a constant")
        else:
            require(life_value == ZERO and occurrence_value == ONE, "once-per-campaign policy needs life0/occurrence1")
    life, occurrence = identity(life_value, method, 'life'), identity(occurrence_value, method, 'occurrence')
    refs = {}
    for label in ('source', 'context', 'actor'):
        value = hook.get(label)
        require(value is None or isinstance(value, str), "semantic Form must be an actual reference variable")
        if value is not None:
            identifier(value)
        refs[label] = value
    require(refs['source'], "missing semantic source Form; text/amount cannot replace it")
    if kind == 0:
        require(refs['actor'] == refs['source'], "kill source must be the same victim reference, not its BaseId")
    indent = re.match(r"\s*", region[0])[0]
    declaration = [indent + 'int RFABCoopBase = ' + match['base']]
    for label, value in refs.items():
        declaration.append(indent + 'int RFABCoop' + label.title() + ' = 0')
        if value:
            expression = '(self as Form)' if value.casefold() == 'self' else value
            declaration += [indent + 'if ' + expression, indent + '    RFABCoop' + label.title() + ' = ' + expression + '.GetFormID()', indent + 'endif']
    region[0] = indent + match['prefix'] + 'RFABCoopBase)'
    condition = (f'if RFABCoopRewards.Submit(RFABCoopRewards.GetWorldEpoch(), {kind}, RFABCoopSource, RFABCoopContext, '
                 f'RFABCoopActor, RFABCoopBase, {life}, {occurrence}) == 0')
    source.lines[start:end + 1] = [*declaration, indent + condition, *["    " + s for s in region], indent + 'endif']


def prepare(profile, source_root, native_root=None):
    require(profile.get('schema') == VERSION, "unsupported installed reward profile schema")
    require(re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]{0,62}", profile.get('name', '')), "invalid installed profile name")
    require(type(profile.get('bizarre')) is bool, "Bizarre is an explicit reviewed profile, not filename detection")
    require(isinstance(profile.get('files'), dict) and 1 <= len(profile['files']) <= 64, "profile needs 1..64 exact source preimages")
    require(len({name.casefold() for name in profile['files']}) == len(profile['files']), 'case-insensitive source filename collision')
    sources, preimages = {}, {}
    for name, spec in profile['files'].items():
        safe_file(name)
        data = read_exact(source_root, name, spec['sha256'])
        preimages[name] = spec['sha256']
        sources[name] = Source(name, data, spec['encoding'])
    require(isinstance(profile.get('native_files'), dict) and len(profile['native_files']) <= 64, "native_files fingerprint set must be explicit and bounded, even if empty for a reviewed script-only profile")
    for name, expected in profile['native_files'].items():
        require(native_root is not None, "native DLL preimages require --data-root")
        require(re.fullmatch(r'SKSE/Plugins/[A-Za-z_][A-Za-z_0-9.-]{0,95}\.dll', name) and '..' not in name,
                'native fingerprint must be a flat SKSE/Plugins DLL path')
        read_exact(native_root, name, expected)
        preimages[name] = expected
    receiver = profile['receiver']
    source = sources[safe_file(receiver['script'])]
    header = next(code(line) for line in source.lines if code(line))
    require(re.search(r"\bextends\s+Quest\b", header, re.I), "reward receiver must be an existing attached Quest script")
    wallet, multiplier, quantum = formula(source, receiver)
    hooks = profile.get('hooks')
    require(isinstance(hooks, list) and 1 <= len(hooks) <= 64, "profile needs explicitly reviewed semantic source regions")
    # Every source-level positive award call in the provided set must be covered or explicitly
    # audited as personal spending; exact file preimages prevent a new call silently slipping in.
    covered = set()
    for hook in hooks:
        target = sources[safe_file(hook['script'])]
        method = target.method(hook['method'])
        begin, end = target.unique_line(method, hook['first']), target.unique_line(method, hook['last'])
        for line in range(begin, end + 1):
            require((target.name, line) not in covered, "overlapping reward source regions")
            covered.add((target.name, line))
    spending = {(safe_file(s['script']), identifier(s['method']).casefold()) for s in profile.get('personal_spending_methods', [])}
    for target in sources.values():
        for method in target.methods().values():
            for i in range(method.start + 1, method.end):
                if re.search(r"\bModXP\s*\(", code(target.lines[i]), re.I):
                    require((target.name, i) in covered or (target.name, method.name.casefold()) in spending,
                            f"uncovered ModXP call in {target.name}::{method.name}; do not silently leave a parallel original award path")
    normalized = json.dumps(profile, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode()
    signature = hashlib.sha256(b'RFAB-source-adapter-v1\0' + normalized).hexdigest()[:16]
    if int(signature, 16) == 0:
        signature = ONE
    kinds = 0
    for hook in hooks:
        target = sources[hook['script']]
        method = forward(target, hook, sources)
        gate(target, method, hook, receiver, source.name)
        kinds |= 1 << hook['kind']
    registrations = profile.get('registrations')
    require(isinstance(registrations, list) and registrations, "receiver must register after actual player load/init via a reviewed lifecycle callback")
    for registration in registrations:
        target = sources[safe_file(registration['script'])]
        method = target.method(registration['method'])
        object_name = identifier(registration['receiver'])
        target.lines.insert(method.start + 1, '    ' + object_name + '.RFABCoopRegister()')
    source.lines += ['', 'bool Function RFABCoopRegister()',
                     f'    return RFABCoopRewards.RegisterAdapter((self as Form), "{profile["name"]}", "{signature}", {kinds}, {str(profile["bizarre"]).lower()}, RFABCoopRewards.GetWorldEpoch())',
                     'EndFunction', '', 'Event OnRFABCoopRewardApply(string ticket, int baseXP)',
                     f'    RFABCoopRewards.RequestPersonalApply(ticket, {wallet}.GetFormID(), {multiplier}.GetFormID(), {quantum})', 'EndEvent']
    notice = receiver.get('level_notification')
    if notice:
        player, flag, function = (identifier(notice[k]) for k in ('player', 'flag', 'function'))
        method = source.method(function)
        require(not re.search(r'ModXP\s*\(|SetPlayerExperience\s*\(|CalculateXP\s*\(', '\n'.join(source.lines[method.start + 1:method.end]), re.I),
                "post-commit level notification must not mutate XP")
        source.lines += ['', 'Event OnRFABCoopRewardApplied(string ticket, int personalFinalXP)',
                         f'    if Game.GetPlayerExperience() >= Game.GetExperienceForLevel({player}.GetLevel()) && {flag}',
                         f'        {flag} = false', f'        {function}()', '    endif', 'EndEvent']
    output = {name: target.bytes() for name, target in sources.items()}
    return output, {'schema': 1, 'profile': profile['name'], 'signature': signature, 'kinds': kinds,
                    'bizarre': profile['bizarre'], 'compiled': False, 'game_validated': False,
                    'coverage': 'reviewed provided source set and explicit native fingerprints; not runtime discovery/proof',
                    'input_sha256': preimages, 'source_sha256': {name: hashlib.sha256(data).hexdigest() for name, data in output.items()}}


def build(profile, source_root, output, native_root=None, compiler=None, imports=(), flags=None):
    data, manifest = prepare(profile, source_root, native_root)
    require(not output.exists() and not output.is_symlink() and output.name not in ('', '.', '..'), "output must be a NEW directory; no in-place installation/overwrite")
    require(output.parent.is_dir(), "output parent must already exist")
    require(not compiler or (compiler.is_file() and flags and flags.is_file() and imports), "real Papyrus compilation needs --compiler, --flags and installed --imports")
    with tempfile.TemporaryDirectory(prefix=output.name + '.staging-', dir=output.parent) as temporary:
        stage = Path(temporary)
        scripts = stage / 'scripts'
        sources = scripts / 'source'
        sources.mkdir(parents=True)
        for name, contents in data.items():
            (sources / name).write_bytes(contents)
        api = ROOT / 'GameFiles/Skyrim/scripts'
        shutil.copyfile(api / 'source/RFABCoopRewards.psc', sources / 'RFABCoopRewards.psc')
        if compiler:
            import_path = ';'.join(str(p.resolve()) for p in (sources, source_root, *imports))
            for name in data:
                result = subprocess.run([str(compiler.resolve()), str((sources / name).resolve()), '-i=' + import_path,
                                         '-o=' + str(scripts.resolve()), '-f=' + str(flags.resolve())],
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', timeout=120)
                pex = scripts / (Path(name).stem + '.pex')
                require(result.returncode == 0 and pex.is_file() and pex.stat().st_size <= 8 * 1024 * 1024 and
                        pex.read_bytes()[:8] == bytes.fromhex('fa57c0de03020001'),
                        f'Papyrus compilation failed for {name}:\n{result.stdout[-16000:]}')
            shutil.copyfile(api / 'RFABCoopRewards.pex', scripts / 'RFABCoopRewards.pex')
            manifest['compiled'] = True
            fingerprints = {f'scripts/{Path(name).stem}.pex': hashlib.sha256((scripts / (Path(name).stem + '.pex')).read_bytes()).hexdigest() for name in data}
            fingerprints['scripts/RFABCoopRewards.pex'] = hashlib.sha256((scripts / 'RFABCoopRewards.pex').read_bytes()).hexdigest()
            fingerprints.update(profile['native_files'])
            manifest['compiled_sha256'] = fingerprints
            table = (f'RFAB_REWARDS_V1\t{manifest["profile"]}\t{manifest["signature"]}\t{manifest["kinds"]}\t{int(manifest["bizarre"])}\tpapyrus-float-v1\n' +
                     ''.join(f'{digest}\t{name}\n' for name, digest in sorted(fingerprints.items())))
            (stage / 'SKSE/Plugins').mkdir(parents=True)
            (stage / 'SKSE/Plugins/RFAB_Rewards.tsv').write_text(table, encoding='ascii', newline='\n')
        else:
            (stage / 'UNCOMPILED-NOT-A-MOD.txt').write_text('Source review output only. Compile against the installed RFAB/game imports. Do not activate as a working MO2 adapter.\n')
        (stage / 'RFAB-reward-profile.json').write_text(json.dumps(manifest, indent=2) + '\n')
        # The selected directory is new; no game/MO2 data or original source is ever replaced.
        os.replace(stage, output)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, required=True)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--data-root', type=Path)
    parser.add_argument('--compiler', type=Path)
    parser.add_argument('--imports', type=Path, action='append', default=[])
    parser.add_argument('--flags', type=Path)
    args = parser.parse_args()
    try:
        manifest = build(json.loads(args.profile.read_text(encoding='utf-8')), args.source_root, args.output,
                         args.data_root, args.compiler, tuple(args.imports), args.flags)
        print(json.dumps(manifest, indent=2))
    except (InvalidProfile, KeyError, TypeError, OSError, UnicodeError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        parser.exit(1, f'installed reward adapter REFUSED: {error}\n')


if __name__ == '__main__':
    main()
