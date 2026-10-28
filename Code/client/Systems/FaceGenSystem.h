#pragma once

#include <cstdint>

struct World;
struct Actor;
struct FaceGenComponent;
struct Tints;

/**
 * @brief Manages per-actor FaceGen face tints and RaceMenu custom hair-tint shader isolation.
 */
struct FaceGenSystem
{
    static void Update(World& aWorld, Actor* apActor, FaceGenComponent& aFaceGenComponent) noexcept;
    static void Setup(World& aWorld, entt::entity aEntity, const Tints& acTints) noexcept;

    static bool TryExtractActorHairColor(Actor* apActor, uint8_t& aOutRed, uint8_t& aOutGreen, uint8_t& aOutBlue) noexcept;
    static void UpdateLocalPlayerHairTintIsolation() noexcept;
    static void InvalidateLocalPlayerHairTintIsolation() noexcept;
};
