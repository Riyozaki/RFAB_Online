#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#include <common/CoopHairColorSync.h>
#include <Messages/AssignCharacterRequest.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Structs/Tints.h>

namespace
{
std::filesystem::path FindV312RepoRoot()
{
    namespace fs = std::filesystem;
    fs::path cur = fs::current_path();
    for (int i = 0; i < 8; ++i)
    {
        if (fs::exists(cur / "Code" / "common" / "CoopHairColorSync.h") &&
            fs::exists(cur / "Code" / "client" / "Systems" / "FaceGenSystem.cpp"))
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

std::string LoadV312RepoFile(const std::filesystem::path& aRoot, const char* apRelPath)
{
    std::ifstream in(aRoot / apRelPath, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

TEST_CASE("v3.1.2 CoopHairColorSync Skyrim SE 1.5.97 RGB <-> BSLightingShaderMaterialHairTint math", "[rfab][v312][haircolor]")
{
    // Exact round-trip across all 256 8-bit color values using Skyrim SE Actor::UpdateHairColor divisor (128.0f)
    for (uint32_t v = 0; v <= 255u; ++v)
    {
        const uint8_t ch = static_cast<uint8_t>(v);
        const float shaderTint = CoopHairColorSync::ChannelToHairShaderTint(ch);
        REQUIRE(CoopHairColorSync::HairShaderTintToChannel(shaderTint) == ch);
    }

    CHECK(CoopHairColorSync::ChannelToHairShaderTint(0u) == Approx(0.0f));
    CHECK(CoopHairColorSync::ChannelToHairShaderTint(128u) == Approx(1.0f));
    CHECK(CoopHairColorSync::ChannelToHairShaderTint(255u) == Approx(255.0f / 128.0f));

    CHECK(CoopHairColorSync::HairShaderTintToChannel(-1.0f) == 0u);
    CHECK(CoopHairColorSync::HairShaderTintToChannel(3.5f) == 255u);
    CHECK(CoopHairColorSync::HairShaderTintToChannel(std::numeric_limits<float>::quiet_NaN()) == 0u);
    CHECK(CoopHairColorSync::HairShaderTintToChannel(std::numeric_limits<float>::infinity()) == 0u);

    // Pack / unpack Player A (Redhead #CC2211) vs Player B (Turquoise #11DDCC)
    const uint32_t packedRedhead = CoopHairColorSync::PackHairColorRgb(0xCCu, 0x22u, 0x11u);
    const uint32_t packedTurquoise = CoopHairColorSync::PackHairColorRgb(0x11u, 0xDDu, 0xCCu);
    REQUIRE(packedRedhead != packedTurquoise);

    uint8_t r = 0, g = 0, b = 0;
    CoopHairColorSync::UnpackHairColorRgb(packedRedhead, r, g, b);
    CHECK(r == 0xCCu);
    CHECK(g == 0x22u);
    CHECK(b == 0x11u);

    CoopHairColorSync::UnpackHairColorRgb(packedTurquoise, r, g, b);
    CHECK(r == 0x11u);
    CHECK(g == 0xDDu);
    CHECK(b == 0xCCu);

    CHECK_FALSE(CoopHairColorSync::IsHairTintDrifted(1.5f, 0.25f, 0.125f, 1.5005f, 0.25f, 0.125f));
    CHECK(CoopHairColorSync::IsHairTintDrifted(1.5f, 0.25f, 0.125f, 0.1328f, 1.726f, 1.593f));
    CHECK(CoopHairColorSync::IsHairTintDrifted(std::numeric_limits<float>::quiet_NaN(), 0.25f, 0.125f, 1.0f, 0.25f, 0.125f));

    int dummyNodeA = 1, dummyNodeB = 2;
    CHECK(CoopHairColorSync::ShouldIsolateHairTintPass(&dummyNodeA, &dummyNodeA, &dummyNodeB, &dummyNodeB, 3u, 3u, false));
    CHECK_FALSE(CoopHairColorSync::ShouldIsolateHairTintPass(&dummyNodeA, &dummyNodeA, &dummyNodeB, &dummyNodeB, 3u, 3u, true));
    CHECK(CoopHairColorSync::ShouldIsolateHairTintPass(&dummyNodeA, &dummyNodeB, &dummyNodeB, &dummyNodeB, 3u, 3u, true));
    CHECK(CoopHairColorSync::ShouldIsolateHairTintPass(&dummyNodeA, &dummyNodeA, &dummyNodeB, &dummyNodeB, 1u, 3u, true));
}

TEST_CASE("v3.1.2 Tints hair color wire round-trip keeps Player A redhead and Player B turquoise isolated", "[rfab][v312][wire]")
{
    AssignCharacterRequest assignRedhead{};
    assignRedhead.Cookie = 101u;
    {
        Tints::Entry freckle{};
        freckle.Name = "Actors\\Character\\Character Assets\\TintMasks\\MaleHeadNord_Freckles.dds";
        freckle.Alpha = 0.65f;
        freckle.Color = 0x00442211u;
        freckle.Type = 6u;
        assignRedhead.FaceTints.Entries.push_back(freckle);
    }
    assignRedhead.FaceTints.SetHairColor(0xCCu, 0x22u, 0x11u);
    REQUIRE(assignRedhead.FaceTints.CountFaceTintEntries() == 1u);
    REQUIRE(assignRedhead.FaceTints.Entries.size() == 2u);

    CharacterSpawnRequest spawnTurquoise{};
    spawnTurquoise.ServerId = 0x2002u;
    spawnTurquoise.PlayerId = 2u;
    spawnTurquoise.IsPlayer = true;
    // Beast / zero-face-tint character with custom RaceMenu hair color
    spawnTurquoise.FaceTints.SetHairColor(0x11u, 0xDDu, 0xCCu);
    // Overwrite hair color in-place without duplicating sentinel entries
    spawnTurquoise.FaceTints.SetHairColor(0x14u, 0xE0u, 0xD0u);
    REQUIRE(spawnTurquoise.FaceTints.CountFaceTintEntries() == 0u);
    REQUIRE(spawnTurquoise.FaceTints.Entries.size() == 1u);

    uint8_t scratchA[4096]{};
    TiltedPhoques::Buffer bufA(scratchA, sizeof(scratchA));
    TiltedPhoques::Buffer::Writer writerA(&bufA);
    assignRedhead.SerializeRaw(writerA);

    AssignCharacterRequest decodedRedhead{};
    TiltedPhoques::Buffer::Reader readerA(&bufA);
    decodedRedhead.DeserializeRaw(readerA);

    uint8_t rA = 0, gA = 0, bA = 0;
    REQUIRE(decodedRedhead.FaceTints.TryGetHairColor(rA, gA, bA));
    CHECK(rA == 0xCCu);
    CHECK(gA == 0x22u);
    CHECK(bA == 0x11u);
    CHECK(decodedRedhead.FaceTints.CountFaceTintEntries() == 1u);

    uint8_t scratchB[4096]{};
    TiltedPhoques::Buffer bufB(scratchB, sizeof(scratchB));
    TiltedPhoques::Buffer::Writer writerB(&bufB);
    spawnTurquoise.SerializeRaw(writerB);

    CharacterSpawnRequest decodedTurquoise{};
    TiltedPhoques::Buffer::Reader readerB(&bufB);
    decodedTurquoise.DeserializeRaw(readerB);

    uint8_t rB = 0, gB = 0, bB = 0;
    REQUIRE(decodedTurquoise.FaceTints.TryGetHairColor(rB, gB, bB));
    CHECK(rB == 0x14u);
    CHECK(gB == 0xE0u);
    CHECK(bB == 0xD0u);
    CHECK(decodedTurquoise.FaceTints.CountFaceTintEntries() == 0u);

    // 260-entry extreme modded tint list still preserves hair color inside the 255-entry wire cap
    Tints overflowTints{};
    for (uint32_t i = 0; i < 260u; ++i)
    {
        Tints::Entry e{};
        e.Name = "TintMask.dds";
        e.Alpha = 0.5f;
        e.Color = i;
        e.Type = 1u;
        overflowTints.Entries.push_back(e);
    }
    overflowTints.SetHairColor(0xFEu, 0x80u, 0x40u);

    uint8_t scratchOverflow[16384]{};
    TiltedPhoques::Buffer bufOv(scratchOverflow, sizeof(scratchOverflow));
    TiltedPhoques::Buffer::Writer writerOv(&bufOv);
    overflowTints.Serialize(writerOv);

    Tints decodedOverflow{};
    TiltedPhoques::Buffer::Reader readerOv(&bufOv);
    decodedOverflow.Deserialize(readerOv);

    uint8_t rOv = 0, gOv = 0, bOv = 0;
    REQUIRE(decodedOverflow.TryGetHairColor(rOv, gOv, bOv));
    CHECK(rOv == 0xFEu);
    CHECK(gOv == 0x80u);
    CHECK(bOv == 0x40u);
}

TEST_CASE("v3.1.2 AppearanceResyncTracker handles deferred skee64 load and mid-session showracemenu cleanly", "[rfab][v312][resync]")
{
    CoopHairColorSync::AppearanceResyncTracker tracker{};

    CoopHairColorSync::Fnv1a64Hasher h1{};
    h1.MixU32(CoopHairColorSync::PackHairColorRgb(0xCCu, 0x22u, 0x11u));
    const uint64_t fpRedhead = h1.Finish();

    CoopHairColorSync::Fnv1a64Hasher h2{};
    h2.MixU32(CoopHairColorSync::PackHairColorRgb(0x11u, 0xDDu, 0xCCu));
    const uint64_t fpTurquoise = h2.Finish();

    // Seeded at connect (t = 1000ms): steady polls with unchanged appearance must not resend
    tracker.RecordSyncedFingerprint(fpRedhead, 1000ULL);
    CHECK_FALSE(tracker.ShouldPollNow(1500ULL, false, false));
    REQUIRE(tracker.ShouldPollNow(2100ULL, false, false));
    CHECK_FALSE(tracker.EvaluateChangedFingerprint(fpRedhead, 2100ULL));

    // Player opens showracemenu at t = 2500ms and drags sliders until t = 4000ms
    CHECK_FALSE(tracker.ShouldPollNow(2500ULL, true, false));
    CHECK_FALSE(tracker.ShouldPollNow(3200ULL, true, false));

    // Player closes showracemenu at t = 4050ms -> immediate poll + single resend!
    REQUIRE(tracker.ShouldPollNow(4050ULL, false, false));
    REQUIRE(tracker.EvaluateChangedFingerprint(fpTurquoise, 4050ULL));
    CHECK_FALSE(tracker.EvaluateChangedFingerprint(fpTurquoise, 5200ULL));
}

TEST_CASE("v3.1.2 Source-level architectural contracts for RaceMenu hair color isolation & v3.1.1 backup", "[rfab][v312][contracts]")
{
    const auto root = FindV312RepoRoot();
    REQUIRE_FALSE(root.empty());

    const std::string buildPs1 = LoadV312RepoFile(root, "Tools/rfab/Build-CloudRelease.ps1");
    const std::string pkgPs1 = LoadV312RepoFile(root, "Tools/rfab/Package-Release.ps1");
    REQUIRE(buildPs1.size() > 3);
    REQUIRE(pkgPs1.size() > 3);
    CHECK(static_cast<unsigned char>(buildPs1[0]) == 0xEF);
    CHECK(static_cast<unsigned char>(buildPs1[1]) == 0xBB);
    CHECK(static_cast<unsigned char>(buildPs1[2]) == 0xBF);
    CHECK(static_cast<unsigned char>(pkgPs1[0]) == 0xEF);
    CHECK(static_cast<unsigned char>(pkgPs1[1]) == 0xBB);
    CHECK(static_cast<unsigned char>(pkgPs1[2]) == 0xBF);
    CHECK(buildPs1.find("[string]$VersionTag = \"v3.1.4\"") != std::string::npos);
    CHECK(buildPs1.find("Ensure-V311BackupRelease") != std::string::npos);
    CHECK(buildPs1.find("\"v3.1.1-backup\"") != std::string::npos);

    const std::string v311Manifest = LoadV312RepoFile(root, "docs/V3.1.1_RELEASE_BACKUP_MANIFEST.json");
    CHECK(v311Manifest.find("\"v3.1.1-backup\"") != std::string::npos);
    CHECK(v311Manifest.find("02c4ec14056034528fb91a570f92f5fa99864da0f3ee6577aea61e26965d7627") != std::string::npos);

    const std::string colorFormH = LoadV312RepoFile(root, "Code/client/Games/Skyrim/Forms/BGSColorForm.h");
    CHECK(colorFormH.find("FormType::ColorForm") != std::string::npos);
    CHECK(colorFormH.find("static_assert(offsetof(BGSColorForm, color) == 0x30)") != std::string::npos);
    CHECK(colorFormH.find("static_assert(sizeof(BGSColorForm) == 0x38)") != std::string::npos);

    const std::string hairMatH = LoadV312RepoFile(root, "Code/client/Games/Skyrim/NetImmerse/BSHairTintShaderMaterial.h");
    CHECK(hairMatH.find("static_assert(offsetof(BSHairTintShaderMaterial, hashKey) == 0x2C)") != std::string::npos);
    CHECK(hairMatH.find("static_assert(offsetof(BSHairTintShaderMaterial, tintRed) == 0xA0)") != std::string::npos);
    CHECK(hairMatH.find("static_assert(sizeof(BSHairTintShaderMaterial) == 0xB0)") != std::string::npos);

    const std::string faceGenCpp = LoadV312RepoFile(root, "Code/client/Systems/FaceGenSystem.cpp");
    CHECK(faceGenCpp.find("IsolateActorHairTintMaterials") != std::string::npos);
    CHECK(faceGenCpp.find("UpdateLocalPlayerHairTintIsolation") != std::string::npos);
    CHECK(faceGenCpp.find("NO_TINT") != std::string::npos);
    CHECK(faceGenCpp.find("pLightingShader->material != pClonedMaterial") != std::string::npos);
    CHECK(faceGenCpp.find("pLightingShader->material != pClonedHairMat") != std::string::npos);
    CHECK(faceGenCpp.find("Memory::Free(pClonedMaterial)") == std::string::npos);

    const std::string assignSubCpp = LoadV312RepoFile(root, "Code/client/Services/Generic/CharacterMovementAndAssignmentSubservice.cpp");
    CHECK(assignSubCpp.find("ApplyRemotePlayerHairColorForm") != std::string::npos);
    CHECK(assignSubCpp.find("m_playerHairColorPool") != std::string::npos);
    CHECK(assignSubCpp.find("message.FaceTints.SetHairColor") != std::string::npos);
    CHECK(assignSubCpp.find("PollLocalPlayerAppearanceSync") != std::string::npos);
}
