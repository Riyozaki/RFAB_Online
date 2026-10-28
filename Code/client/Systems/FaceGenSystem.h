#pragma once

#include <cstdint>
#include <vector>

#include <Structs/Tints.h>

struct World;
struct Actor;
struct FaceGenComponent;

struct FaceGenMainThreadEntry
{
    uint32_t FormId{0};
    Tints FaceTints{};
};

/**
 * @brief Manages per-actor FaceGen face tints and RaceMenu custom hair-tint shader isolation.
 *
 * All NetImmerse scene-graph traversal, D3D11 tint texture creation, and `BSShaderMaterial` updates
 * execute strictly on Skyrim's real `WinMain`/D3D11 main thread (`HookMainLoop` SE 35565), never on
 * `BSJobs` Papyrus worker threads (`HookVMUpdate` SE 53115) and never while offline.
 */
struct FaceGenSystem
{
    static void Update(World& aWorld, Actor* apActor, FaceGenComponent& aFaceGenComponent) noexcept;
    static void Setup(World& aWorld, entt::entity aEntity, const Tints& acTints) noexcept;

    static void ExecuteMainThreadPass(
        const std::vector<FaceGenMainThreadEntry>& acRemoteActors,
        bool aHasRemoteHairPeers) noexcept;

    static bool TryExtractActorHairColor(Actor* apActor, uint8_t& aOutRed, uint8_t& aOutGreen, uint8_t& aOutBlue) noexcept;
    static void UpdateLocalPlayerHairTintIsolation() noexcept;
    static void InvalidateLocalPlayerHairTintIsolation() noexcept;
};
