#!/usr/bin/env python3
"""
Regression test suite for RFAB Online v1.2.0-hotfix.1:
  1. Verifies immutable v1.2.0 release backup manifest & Build-CloudRelease.ps1 clobber guards.
  2. Verifies UTF-8 BOM on all Tools/rfab/*.ps1 scripts (Windows PowerShell 5.1 Cyrillic safety).
  3. Verifies WeatherService non-leader StatsMenu/CustomMenu/interior ForceWeather guards.
  4. Verifies MO2 ModOrganizer.ini gamePath= priority in PathSelection.cpp & UNICODE_STRING bounds in FileMapping.cpp.
  5. Verifies D3D11 Render-Thread isolation of RfabAdapterService::OnDrawTacticalHud().
  6. Verifies version-agnostic StatsMenu pause detection & SkyrimSoulsRE.ini bStatsMenu=false self-healer.
"""
import importlib.util
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main() -> int:
    # 1. Backup manifest & release script protection
    manifest_path = ROOT / "docs" / "V1.2.0_RELEASE_BACKUP_MANIFEST.json"
    assert manifest_path.is_file(), f"Missing {manifest_path}"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert manifest["tag_name"] == "v1.2.0"
    assert manifest["target_commitish"] == "b431be0da38764ce1f4d658329934b4a8ccc5692"
    assert len(manifest["assets"]) == 4

    build_ps1 = (ROOT / "Tools" / "rfab" / "Build-CloudRelease.ps1").read_bytes()
    assert build_ps1.startswith(b"\xef\xbb\xbf"), "Build-CloudRelease.ps1 must retain UTF-8 BOM"
    build_txt = build_ps1[3:].decode("utf-8")
    for tag in ("v1.0.0", "v1.1.0", "v1.2.0", "v1.2.0-backup"):
        assert f'"{tag}"' in build_txt, f"Build-CloudRelease.ps1 must protect {tag}"

    for ps1 in sorted((ROOT / "Tools" / "rfab").glob("*.ps1")):
        assert ps1.read_bytes().startswith(b"\xef\xbb\xbf"), f"Missing UTF-8 BOM on {ps1}"

    # 2. WeatherService non-leader StatsMenu CTD fix
    weather_cpp = (ROOT / "Code" / "client" / "Services" / "Generic" / "WeatherService.cpp").read_text(encoding="utf-8")
    assert "CanMutateOutdoorSkyWeather" in weather_cpp
    assert 'BSFixedString("StatsMenu")' in weather_cpp
    assert 'BSFixedString("CustomMenu")' in weather_cpp
    assert "IsLocalPlayerInExteriorWorldspace" in weather_cpp
    assert "else\n            SetCachedWeather();" not in weather_cpp.replace("\r\n", "\n")

    # 3. MO2 ModOrganizer.ini gamePath= priority & FileMapping UNICODE_STRING byte-length fix
    path_sel_cpp = (ROOT / "Code" / "immersive_launcher" / "oobe" / "PathSelection.cpp").read_text(encoding="utf-8")
    assert "RfabCompat::ResolveMo2ConfiguredGamePath" in path_sel_cpp

    file_map_cpp = (ROOT / "Code" / "immersive_launcher" / "stubs" / "FileMapping.cpp").read_text(encoding="utf-8")
    assert "DllName->Length / sizeof(wchar_t)" in file_map_cpp
    assert "std::wcsncmp(TARGET_NAME L\".exe\", DllName->Buffer, DllName->Length)" not in file_map_cpp

    # 4. RfabAdapterService::OnDrawTacticalHud render-thread snapshot isolation
    rfab_cpp = (ROOT / "Code" / "client" / "Services" / "RfabAdapterService.cpp").read_text(encoding="utf-8")
    assert "RefreshTacticalHudSnapshot();" in rfab_cpp
    draw_idx = rfab_cpp.find("void RfabAdapterService::OnDrawTacticalHud() noexcept")
    assert draw_idx != -1
    draw_body = rfab_cpp[draw_idx:]
    assert "m_tacticalHudSnapshotMutex" in draw_body
    for forbidden in ("BSFixedString", "GetMenuOpen", "m_world.view", "TESForm::GetById", "WorldPtToScreenPt3", "GetPartyService"):
        assert forbidden not in draw_body, f"Render-thread hazard '{forbidden}' found inside OnDrawTacticalHud!"

    # 5. Version-agnostic StatsMenu pause detection & SkyrimSoulsRE.ini bStatsMenu = false patcher
    skills_cpp = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "Interface" / "Menus" / "SkillsMenu.cpp").read_text(encoding="utf-8")
    ui_cpp = (ROOT / "Code" / "client" / "Games" / "Skyrim" / "Interface" / "UI.cpp").read_text(encoding="utf-8")
    assert "HasCustomSkillsOrRfabFilesUnderData" in skills_cpp
    assert "ShouldKeepStatsMenuPausedRuntime()" in ui_cpp

    spec = importlib.util.spec_from_file_location(
        "patch_rfab_modpack_compat", ROOT / "Tools" / "rfab" / "patch_rfab_modpack_compat.py"
    )
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    patched_souls, changed = mod.patch_skyrim_souls_ini(
        "[UNPAUSED_MENUS]\nbStatsMenu = true\nbCustomMenu = true\n[SLOWMOTION]\nfSlowMotionMultiplier = 0.25\n"
    )
    assert changed
    assert "bStatsMenu = false" in patched_souls
    assert "bCustomMenu = true" in patched_souls
    assert "fSlowMotionMultiplier = 1.000000" in patched_souls

    print("[test_v120_hotfix_regressions] OK: All v1.2.0-hotfix.1 regression checks passed!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
