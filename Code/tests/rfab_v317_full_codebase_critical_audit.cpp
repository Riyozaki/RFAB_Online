#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/ViewBuffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <Messages/StringCacheUpdate.h>
#include <Messages/NotifyFactionsChanges.h>
#include <Messages/NotifyObjectInventoryChanges.h>
#include <Messages/RequestObjectInventoryChanges.h>
#include <Messages/NotifyPlayerList.h>
#include <common/CoopPacketSafety.h>
#include <common/CoopWorldStabilizer.h>

namespace
{
std::filesystem::path FindV317RepoRoot()
{
    namespace fs = std::filesystem;
    fs::path cur = fs::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (fs::exists(cur / "Code" / "common" / "CoopWorldStabilizer.h") &&
            fs::exists(cur / "Code" / "common" / "CoopPacketSafety.h") &&
            fs::exists(cur / "Code" / "client" / "Systems" / "ModSystem.cpp"))
        {
            return cur;
        }
        if (!cur.has_parent_path())
            break;
        fs::path parent = cur.parent_path();
        if (parent == cur)
            break;
        cur = parent;
    }
    return {};
}

std::string LoadV317RepoFile(const std::filesystem::path& aRoot, const char* apRelPath)
{
    std::ifstream in(aRoot / apRelPath, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST_CASE("v3.1.7 ModSystem FormID isolation, calendar/waypoint sanitization & save-buffer safety", "[rfab][v317][safety]")
{
    // 1. Standard FormID translation: unmapped local plugins must NEVER masquerade as Skyrim.esm (server ModId 0)!
    uint32_t standardToServer[0x100];
    std::fill_n(standardToServer, 0x100, CoopPacketSafety::kUnmappedServerModId);
    standardToServer[0xFF] = CoopPacketSafety::kRuntimeDynamicModId;
    standardToServer[0x00] = 0u;   // Skyrim.esm -> server ModId 0
    standardToServer[0x04] = 17u;  // Dragonborn.esm -> server ModId 17
    // Slot 0x2A is a local-only client mod NOT loaded on the server (remains kUnmappedServerModId)

    uint32_t modId = 999u;
    uint32_t baseId = 999u;

    // Null FormID (0x00000000) translates cleanly to {0, 0} -> true even before ModList handshake
    uint32_t preHandshakeTable[0x100];
    std::fill_n(preHandshakeTable, 0x100, CoopPacketSafety::kUnmappedServerModId);
    preHandshakeTable[0xFF] = CoopPacketSafety::kRuntimeDynamicModId;
    CHECK(CoopPacketSafety::TranslateStandardFormToServerModId(0u, preHandshakeTable, modId, baseId));
    CHECK(modId == 0u);
    CHECK(baseId == 0u);

    // Pre-handshake Skyrim.esm non-zero form (0x0000003C Tamriel) is rejected cleanly instead of leaking bogus {0, 0x3C}
    CHECK_FALSE(CoopPacketSafety::TranslateStandardFormToServerModId(0x0000003Cu, preHandshakeTable, modId, baseId));
    CHECK(modId == 0u);
    CHECK(baseId == 0u);

    // Mapped Skyrim.esm FormID (0x0000000F Gold001) -> {0, 0xF} true
    CHECK(CoopPacketSafety::TranslateStandardFormToServerModId(0x0000000Fu, standardToServer, modId, baseId));
    CHECK(modId == 0u);
    CHECK(baseId == 0x0Fu);

    // Mapped DLC FormID (0x04017F86) -> {17, 0x017F86} true
    CHECK(CoopPacketSafety::TranslateStandardFormToServerModId(0x04017F86u, standardToServer, modId, baseId));
    CHECK(modId == 17u);
    CHECK(baseId == 0x017F86u);

    // Unmapped local-only ESP FormID (0x2A005901) -> rejected (false, {0, 0}) instead of corrupting into Skyrim.esm 0x00005901!
    CHECK_FALSE(CoopPacketSafety::TranslateStandardFormToServerModId(0x2A005901u, standardToServer, modId, baseId));
    CHECK(modId == 0u);
    CHECK(baseId == 0u);

    // Runtime dynamic 0xFFxxxxxx FormID -> {0xFFFFFFFF, baseId} true
    CHECK(CoopPacketSafety::TranslateStandardFormToServerModId(0xFF00ABCDu, standardToServer, modId, baseId));
    CHECK(modId == CoopPacketSafety::kRuntimeDynamicModId);
    CHECK(baseId == 0x0000ABCDu);

    // 2. Calendar sanitization & NaN-safe host calendar fields
    {
        float timeScale = std::numeric_limits<float>::quiet_NaN();
        float gameHour = -3.5f;
        uint32_t day = 99u;
        uint32_t month = 55u;
        uint32_t year = 99999u;
        CoopPacketSafety::SanitizeSkyrimCalendarValues(timeScale, gameHour, day, month, year);
        CHECK(timeScale == Approx(20.0f));
        CHECK(gameHour == Approx(20.5f));
        CHECK(day == 17u);
        CHECK(month == 7u);
        CHECK(year == 201u);

        float nanHour = std::numeric_limits<float>::infinity();
        CoopPacketSafety::SanitizeSkyrimCalendarValues(timeScale, nanHour, day, month, year);
        CHECK(nanHour == Approx(8.0f));
    }

    // 3. Waypoint & teleport world coordinate validation
    struct DummyVec3
    {
        float x;
        float y;
        float z;
    };
    CHECK(CoopPacketSafety::AreWorldCoordinatesValid(12000.0f, -45000.0f, 3200.0f));
    CHECK(CoopPacketSafety::IsFiniteVec3(DummyVec3{12000.0f, -45000.0f, 3200.0f}));
    CHECK_FALSE(CoopPacketSafety::AreWorldCoordinatesValid(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f));
    CHECK_FALSE(CoopPacketSafety::IsFiniteVec3(DummyVec3{CoopPacketSafety::kWorldCoordinateMaxAbs + 100.0f, 0.0f, 0.0f}));

    // 4. BGSSaveFormBuffer::WriteId capacity guard & SpellCast / Subtitle hook predicates
    char dummyBuf[32]{};
    CHECK_FALSE(CoopWorldStabilizer::CanWriteFormIdVarIntsToSaveBuffer(nullptr, 0, 32));
    CHECK_FALSE(CoopWorldStabilizer::CanWriteFormIdVarIntsToSaveBuffer(dummyBuf, 24, 32));
    CHECK(CoopWorldStabilizer::CanWriteFormIdVarIntsToSaveBuffer(dummyBuf, 22, 32));

    // Remote spell cast must be blocked ONLY when NOT inside ScopedSpellCastOverride!
    CHECK_FALSE(CoopWorldStabilizer::ShouldAllowHookSpellCastExecution(true, false));
    CHECK(CoopWorldStabilizer::ShouldAllowHookSpellCastExecution(true, true));
    CHECK(CoopWorldStabilizer::ShouldAllowHookSpellCastExecution(false, false));
    CHECK(CoopWorldStabilizer::ShouldTriggerLocalSpellCastEvent(true, false, true));
    CHECK_FALSE(CoopWorldStabilizer::ShouldTriggerLocalSpellCastEvent(true, true, true));
    CHECK_FALSE(CoopWorldStabilizer::ShouldTriggerLocalSpellCastEvent(false, false, true));

    // Subtitles from remote puppets or player characters must never echo back into Speaker->Server broadcast
    CHECK_FALSE(CoopWorldStabilizer::ShouldTriggerSubtitleSync(true, true, true, false));
    CHECK_FALSE(CoopWorldStabilizer::ShouldTriggerSubtitleSync(true, true, false, true));
    CHECK_FALSE(CoopWorldStabilizer::ShouldTriggerSubtitleSync(false, true, false, false));
    CHECK(CoopWorldStabilizer::ShouldTriggerSubtitleSync(true, true, false, false));
}

TEST_CASE("v3.1.7 Truncated wire packet EOF resilience across StringCache, Factions, ObjectInventory & PlayerList", "[rfab][v317][encoding]")
{
    using TiltedPhoques::Buffer;
    using TiltedPhoques::Serialization;
    using TiltedPhoques::ViewBuffer;

    // Truncated StringCacheUpdate claiming 50,000 strings with 0 payload bytes must NOT allocate 50,000 empty strings!
    {
        uint8_t raw[64]{};
        ViewBuffer writeBuf(raw, sizeof(raw));
        Buffer::Writer writer(&writeBuf);
        Serialization::WriteVarInt(writer, 7u);      // StartId
        Serialization::WriteVarInt(writer, 50000u);  // Claimed count with immediate EOF

        ViewBuffer readBuf(raw, writer.Size());
        Buffer::Reader reader(&readBuf);
        StringCacheUpdate msg;
        msg.DeserializeRaw(reader);
        CHECK(msg.StartId == 7u);
        CHECK(msg.Values.empty());
    }

    // Truncated NotifyPlayerList claiming 4,000 players with 0 payload bytes must stop at EOF immediately
    {
        uint8_t raw[64]{};
        ViewBuffer writeBuf(raw, sizeof(raw));
        Buffer::Writer writer(&writeBuf);
        Serialization::WriteVarInt(writer, 4000u);

        ViewBuffer readBuf(raw, writer.Size());
        Buffer::Reader reader(&readBuf);
        NotifyPlayerList msg;
        msg.DeserializeRaw(reader);
        CHECK(msg.Players.empty());
    }

    // Truncated NotifyFactionsChanges claiming 500 changes with 0 payload bytes must stop at EOF immediately
    {
        uint8_t raw[64]{};
        ViewBuffer writeBuf(raw, sizeof(raw));
        Buffer::Writer writer(&writeBuf);
        Serialization::WriteVarInt(writer, 500u);

        ViewBuffer readBuf(raw, writer.Size());
        Buffer::Reader reader(&readBuf);
        NotifyFactionsChanges msg;
        msg.DeserializeRaw(reader);
        CHECK(msg.Changes.empty());
    }

    // Normal StringCacheUpdate round-trip still preserves every entry 1:1
    {
        StringCacheUpdate src;
        src.StartId = 42;
        src.Values = {"Skyrim.esm", "RFAB.esp", "ActorBehavior"};

        uint8_t raw[512]{};
        ViewBuffer writeBuf(raw, sizeof(raw));
        Buffer::Writer writer(&writeBuf);
        src.SerializeRaw(writer);

        ViewBuffer readBuf(raw, writer.Size());
        Buffer::Reader reader(&readBuf);
        StringCacheUpdate dst;
        dst.DeserializeRaw(reader);
        CHECK(dst == src);
    }
}

TEST_CASE("v3.1.7 Full-codebase critical audit source contracts across Client, Server, Encoding & Launcher", "[rfab][v317][contracts]")
{
    const auto root = FindV317RepoRoot();
    REQUIRE_FALSE(root.empty());

    // 1. Release script protection (`v3.1.6` preserved in $ProtectedTags, bumped to `v3.1.7`, UTF-8 BOM intact)
    const std::string buildPs1 = LoadV317RepoFile(root, "Tools/rfab/Build-CloudRelease.ps1");
    const std::string pkgPs1 = LoadV317RepoFile(root, "Tools/rfab/Package-Release.ps1");
    REQUIRE(buildPs1.size() > 3);
    REQUIRE(pkgPs1.size() > 3);
    CHECK(static_cast<unsigned char>(buildPs1[0]) == 0xEF);
    CHECK(static_cast<unsigned char>(buildPs1[1]) == 0xBB);
    CHECK(static_cast<unsigned char>(buildPs1[2]) == 0xBF);
    CHECK(static_cast<unsigned char>(pkgPs1[0]) == 0xEF);
    CHECK(static_cast<unsigned char>(pkgPs1[1]) == 0xBB);
    CHECK(static_cast<unsigned char>(pkgPs1[2]) == 0xBF);
    CHECK(buildPs1.find("Resolve-ReleaseVersionTag") != std::string::npos);
    CHECK(pkgPs1.find("Resolve-ReleaseVersionTag") != std::string::npos);
    CHECK(buildPs1.find("\"v3.1.6\"") != std::string::npos);

    // 2. ModManager + ModSystem + ModsComponent architectural FormID isolation
    const std::string modMgrCpp = LoadV317RepoFile(root, "Code/client/Games/ModManager.cpp");
    const std::string modSysCpp = LoadV317RepoFile(root, "Code/client/Systems/ModSystem.cpp");
    const std::string modsCompCpp = LoadV317RepoFile(root, "Code/server/Components/ModsComponent.cpp");
    CHECK(modMgrCpp.find("if (!acpName || acpName[0] == '\\0')") != std::string::npos);
    CHECK(modMgrCpp.find("pEntry->data->IsLoaded()") != std::string::npos);
    CHECK(modSysCpp.find("CoopPacketSafety::kUnmappedServerModId") != std::string::npos);
    CHECK(modSysCpp.find("TranslateStandardFormToServerModId") != std::string::npos);
    CHECK(modSysCpp.find("if (aServerId == 0u && aFormId == 0u)") != std::string::npos);
    CHECK(modSysCpp.find("ModSystem::OnDisconnected") != std::string::npos);
    CHECK(modsCompCpp.find("NormalizeModFilenameKey") != std::string::npos);

    // 3. TESNPC::Serialize heap-safe TLS buffer + SaveLoad bounds + Russian string truncation & null-ptr guards
    const std::string formsCpp = LoadV317RepoFile(root, "Code/client/Games/Forms.cpp");
    const std::string saveLoadCpp = LoadV317RepoFile(root, "Code/client/Games/SaveLoad.cpp");
    const std::string discordCpp = LoadV317RepoFile(root, "Code/client/Services/Generic/DiscordService.cpp");
    const std::string effectItemCpp = LoadV317RepoFile(root, "Code/client/Games/Skyrim/Magic/EffectItem.cpp");
    CHECK(formsCpp.find("kNpcSerializeScratchCapacity = 1u << 18") != std::string::npos);
    CHECK(formsCpp.find("char buffer[1 << 15]") == std::string::npos);
    CHECK(saveLoadCpp.find("CanWriteFormIdVarIntsToSaveBuffer") != std::string::npos);
    CHECK(discordCpp.find("_TRUNCATE") != std::string::npos);
    CHECK(effectItemCpp.find("return pEffectSetting && pEffectSetting->eArchetype == EffectArchetypes::ArchetypeID::kValueModifier") != std::string::npos);

    // 4. Client hooks & services (BSScript, SubtitleManager, ActorMagicCaster, PlayerService GodMode, PartyService leadership, MagicService F4)
    const std::string bsScriptCpp = LoadV317RepoFile(root, "Code/client/Games/Misc/BSScript.cpp");
    const std::string subMgrCpp = LoadV317RepoFile(root, "Code/client/Games/Misc/SubtitleManager.cpp");
    const std::string magicCasterCpp = LoadV317RepoFile(root, "Code/client/Games/Skyrim/Magic/ActorMagicCaster.cpp");
    const std::string cliPlayerCpp = LoadV317RepoFile(root, "Code/client/Services/Generic/PlayerService.cpp");
    const std::string cliPartyCpp = LoadV317RepoFile(root, "Code/client/Services/Generic/PartyService.cpp");
    const std::string cliWeatherCpp = LoadV317RepoFile(root, "Code/client/Services/Generic/WeatherService.cpp");
    const std::string cliMagicCpp = LoadV317RepoFile(root, "Code/client/Services/Generic/MagicService.cpp");
    CHECK(bsScriptCpp.find("if (World::IsCreated() && apFunction && apFunction->functionAddress)") != std::string::npos);
    CHECK(subMgrCpp.find("ShouldTriggerSubtitleSync") != std::string::npos);
    CHECK(magicCasterCpp.find("ShouldAllowHookSpellCastExecution") != std::string::npos);
    CHECK(magicCasterCpp.find("apSpell ? apSpell : apThis->pCurrentSpell") != std::string::npos);
    CHECK(cliPlayerCpp.find("PlayerCharacter::SetGodMode(false);") != std::string::npos);
    CHECK(cliPartyCpp.find("OnPartyLeadershipChanged") != std::string::npos);
    CHECK(cliWeatherCpp.find("void WeatherService::OnPartyLeadershipChanged(bool aIsLeader) noexcept") != std::string::npos);
    CHECK(cliMagicCpp.find("MagicService::OnDisconnected") != std::string::npos);
    CHECK(cliMagicCpp.find("GetForegroundWindow()") != std::string::npos);

    // 5. Server services, wire bounds & Launcher startup error code / CLI parsing
    const std::string srvPartyCpp = LoadV317RepoFile(root, "Code/server/Services/PartyService.cpp");
    const std::string srvOverlayCpp = LoadV317RepoFile(root, "Code/server/Services/OverlayService.cpp");
    const std::string srvSocialCpp = LoadV317RepoFile(root, "Code/server/Services/CharacterSocialAndMountSubservice.cpp");
    const std::string srvAvCpp = LoadV317RepoFile(root, "Code/server/Services/ActorValueService.cpp");
    const std::string srvPlayerCpp = LoadV317RepoFile(root, "Code/server/Services/PlayerService.cpp");
    const std::string launcherCpp = LoadV317RepoFile(root, "Code/immersive_launcher/Launcher.cpp");
    CHECK(srvPartyCpp.find("m_parties.find(*partyComp.JoinedPartyId) != m_parties.end()") != std::string::npos);
    CHECK(srvOverlayCpp.find("StripHtmlTagsNoThrow") != std::string::npos);
    CHECK(srvOverlayCpp.find("std::regex") == std::string::npos);
    CHECK(srvSocialCpp.find("pOwner->GetOwner() != acMessage.pPlayer") != std::string::npos);
    CHECK(srvAvCpp.find("kActorValueCount = 164") != std::string::npos);
    CHECK(srvPlayerCpp.find("std::clamp(rawGoldLossFactor, 0.0f, 1.0f)") != std::string::npos);
    CHECK(launcherCpp.find("DIE_STARTUP_NOW") != std::string::npos);
    CHECK(launcherCpp.find("oobe::PathArgument(aArgv[++i])") != std::string::npos);
}
