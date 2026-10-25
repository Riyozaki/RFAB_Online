#include <catch2/catch.hpp>

#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <common/CoopInventoryDiff.h>
#include <common/CoopPacketSafety.h>

#include <Messages/AssignObjectsRequest.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/ClientReferencesMoveRequest.h>
#include <Messages/NotifyActorMaxValueChanges.h>
#include <Messages/NotifyActorValueChanges.h>
#include <Messages/NotifyPartyInfo.h>
#include <Messages/RequestActorMaxValueChanges.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/RequestFactionsChanges.h>
#include <Messages/ServerReferencesMoveRequest.h>
#include <Messages/ShiftGridCellRequest.h>
#include <Messages/SpellCastRequest.h>
#include <Structs/ActionReplayChain.h>
#include <Structs/ActorValues.h>
#include <Structs/AnimationVariables.h>
#include <Structs/Factions.h>
#include <Structs/Inventory.h>
#include <Structs/Mods.h>
#include <Structs/QuestLog.h>
#include <Structs/ReferenceUpdate.h>
#include <Structs/Tints.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace TiltedPhoques;

TEST_CASE("v2.0.0 SkyMP differential inventory reconciliation avoids RemoveAllItems strip", "[rfab][v200][inventory]")
{
    Inventory::Entry fortifyArmor{};
    fortifyArmor.BaseId = GameId{2, 0x012345};
    fortifyArmor.Count = 1;
    fortifyArmor.ExtraHealth = 1.25f;
    fortifyArmor.ExtraEnchantId = GameId{2, 0x054321};
    fortifyArmor.ExtraWorn = true;

    Inventory::Entry healingPotion{};
    healingPotion.BaseId = GameId{0, 0x03EADD};
    healingPotion.Count = 10;

    Inventory::Entry questAmulet{};
    questAmulet.BaseId = GameId{0, 0x0CC846};
    questAmulet.Count = 1;
    questAmulet.IsQuestItem = true;

    SECTION("Identical inventory produces an exact NoOp delta (zero armor strip)")
    {
        std::vector<Inventory::Entry> current{fortifyArmor, healingPotion};
        std::vector<Inventory::Entry> target{fortifyArmor, healingPotion};

        const auto plan = CoopInventoryDiff::ComputeInventoryDelta<Inventory::Entry>(current, target, false);
        REQUIRE(plan.IsNoOp());
    }

    SECTION("Consuming potions removes only the consumed count while leaving worn armor untouched")
    {
        std::vector<Inventory::Entry> current{fortifyArmor, healingPotion};
        Inventory::Entry remainingPotions = healingPotion;
        remainingPotions.Count = 7;
        std::vector<Inventory::Entry> target{fortifyArmor, remainingPotions};

        const auto plan = CoopInventoryDiff::ComputeInventoryDelta<Inventory::Entry>(current, target, false);
        REQUIRE(plan.ItemsToAdd.empty());
        REQUIRE(plan.WornStateChanges.empty());
        REQUIRE(plan.ItemsToRemove.size() == 1);
        REQUIRE(plan.ItemsToRemove[0].BaseId == healingPotion.BaseId);
        REQUIRE(plan.ItemsToRemove[0].Count == -3);
    }

    SECTION("Equipping or unequipping an already-held shield emits only a worn-state transition")
    {
        Inventory::Entry shieldUnworn{};
        shieldUnworn.BaseId = GameId{0, 0x013955};
        shieldUnworn.Count = 1;
        shieldUnworn.ExtraWornLeft = false;

        Inventory::Entry shieldWorn = shieldUnworn;
        shieldWorn.ExtraWornLeft = true;

        const auto equipPlan = CoopInventoryDiff::ComputeInventoryDelta<Inventory::Entry>(
            std::vector<Inventory::Entry>{shieldUnworn},
            std::vector<Inventory::Entry>{shieldWorn},
            false);
        REQUIRE(equipPlan.ItemsToRemove.empty());
        REQUIRE(equipPlan.ItemsToAdd.empty());
        REQUIRE(equipPlan.WornStateChanges.size() == 1);
        REQUIRE(equipPlan.WornStateChanges[0].Action == CoopInventoryDiff::EquipActionKind::Equip);
        REQUIRE(equipPlan.WornStateChanges[0].Item.ExtraWornLeft == true);

        const auto unequipPlan = CoopInventoryDiff::ComputeInventoryDelta<Inventory::Entry>(
            std::vector<Inventory::Entry>{shieldWorn},
            std::vector<Inventory::Entry>{shieldUnworn},
            false);
        REQUIRE(unequipPlan.ItemsToRemove.empty());
        REQUIRE(unequipPlan.ItemsToAdd.empty());
        REQUIRE(unequipPlan.WornStateChanges.size() == 1);
        REQUIRE(unequipPlan.WornStateChanges[0].Action == CoopInventoryDiff::EquipActionKind::Unequip);
    }

    SECTION("Retaining quest items never deletes or duplicates held quest items")
    {
        std::vector<Inventory::Entry> current{fortifyArmor, questAmulet};
        std::vector<Inventory::Entry> target{fortifyArmor};

        const auto plan = CoopInventoryDiff::ComputeInventoryDelta<Inventory::Entry>(current, target, true);
        REQUIRE(plan.IsNoOp());
    }

    SECTION("Tempered vs untempered weapons with same BaseId are reconciled as distinct physical items")
    {
        Inventory::Entry rawSword{};
        rawSword.BaseId = GameId{0, 0x013989};
        rawSword.Count = 1;
        rawSword.ExtraHealth = 1.0f;

        Inventory::Entry temperedSword = rawSword;
        temperedSword.ExtraHealth = 1.5f;
        temperedSword.ExtraWorn = true;

        const auto plan = CoopInventoryDiff::ComputeInventoryDelta<Inventory::Entry>(
            std::vector<Inventory::Entry>{rawSword},
            std::vector<Inventory::Entry>{temperedSword},
            false);
        REQUIRE(plan.ItemsToRemove.size() == 1);
        REQUIRE(plan.ItemsToRemove[0].Count == -1);
        REQUIRE(plan.ItemsToAdd.size() == 1);
        REQUIRE(plan.ItemsToAdd[0].ExtraHealth == Approx(1.5f));
        REQUIRE(plan.ItemsToAdd[0].ExtraWorn == true);
    }
}

TEST_CASE("v2.0.0 wire packet bounds and hostile/truncated payload resilience", "[rfab][v200][security]")
{
    SECTION("Hostile VarInt counts are bounded and exit cleanly on EOF without throwing or signed-overflow")
    {
        Buffer buf(128);
        Buffer::Writer writer(&buf);
        // Write an astronomical VarInt (> INT32_MAX and > UINT32_MAX) with zero trailing payload bytes
        Serialization::WriteVarInt(writer, 0x90000000ULL);
        Serialization::WriteVarInt(writer, 0x90000000ULL);
        Buffer truncatedPacket(writer.Size());
        std::memcpy(truncatedPacket.GetWriteData(), buf.GetData(), writer.Size());

        {
            Buffer::Reader reader(&truncatedPacket);
            Inventory inv{};
            inv.Deserialize(reader);
            REQUIRE(inv.Entries.size() <= 2);
        }

        {
            Buffer::Reader reader(&truncatedPacket);
            ActorValues av{};
            av.Deserialize(reader);
            REQUIRE(av.ActorValuesList.size() <= 2);
            REQUIRE(av.ActorMaxValuesList.size() <= 2);
        }

        {
            Buffer::Reader reader(&truncatedPacket);
            Factions factions{};
            // Previously threw std::runtime_error inside noexcept DeserializeRaw -> std::terminate()!
            REQUIRE_NOTHROW(factions.Deserialize(reader));
            REQUIRE(factions.NpcFactions.size() <= 2);
        }

        {
            Buffer::Reader reader(&truncatedPacket);
            AnimationVariables anim{};
            REQUIRE_NOTHROW(anim.ApplyDiff(reader));
            REQUIRE(anim.Booleans.size() <= CoopPacketSafety::kMaxAnimVars);
            REQUIRE(anim.Integers.size() <= CoopPacketSafety::kMaxAnimVars);
            REQUIRE(anim.Floats.size() <= CoopPacketSafety::kMaxAnimVars);
        }
    }

    SECTION("Oversized collections clamp cleanly during Serialize without corrupting bitstream")
    {
        AssignObjectsRequest req{};
        req.Objects.resize(300); // > 8-bit 255 wire limit
        for (uint32_t i = 0; i < 300; ++i)
            req.Objects[i].Id = GameId{1, i + 1};

        Buffer buf(65536);
        Buffer::Writer writer(&buf);
        req.SerializeRaw(writer);

        Buffer::Reader reader(&buf);
        AssignObjectsRequest decoded{};
        // AssignObjectsRequest::DeserializeRaw calls ClientMessage::DeserializeRaw (0 bits), so SerializeRaw matches
        decoded.DeserializeRaw(reader);
        REQUIRE(decoded.Objects.size() == 255);
        REQUIRE(decoded.Objects.front().Id == GameId{1, 1});
        REQUIRE(decoded.Objects.back().Id == GameId{1, 255});
    }

    SECTION("Float & item count sanitizers block NaN/Inf and extreme stack counts")
    {
        const float nanVal = std::numeric_limits<float>::quiet_NaN();
        const float infVal = std::numeric_limits<float>::infinity();

        REQUIRE(CoopPacketSafety::SanitizeFiniteFloat(nanVal, -100.f, 100.f, 1.0f) == Approx(1.0f));
        REQUIRE(CoopPacketSafety::SanitizeFiniteFloat(infVal, -100.f, 100.f, 1.0f) == Approx(1.0f));
        REQUIRE(CoopPacketSafety::SanitizeFiniteFloat(5000.f, -100.f, 100.f, 1.0f) == Approx(100.0f));

        REQUIRE(CoopPacketSafety::SanitizeItemCountDelta(2'000'000'000) == 1'000'000);
        REQUIRE(CoopPacketSafety::SanitizeItemCountDelta(-2'000'000'000) == -1'000'000);
        REQUIRE(CoopPacketSafety::SanitizeItemCountDelta(-3) == -3);
    }

    SECTION("Default-constructed messages zero-initialize all primitive fields")
    {
        SpellCastRequest spellReq{};
        REQUIRE(spellReq.CasterId == 0);
        REQUIRE(spellReq.DesiredTarget == 0);
        REQUIRE(spellReq.IsDualCasting == false);

        Mods::Entry modEntry{};
        REQUIRE(modEntry.Id == 0);
        REQUIRE(modEntry.IsLite == false);

        Tints::Entry tintEntry{};
        REQUIRE(tintEntry.Type == 0);
        REQUIRE(tintEntry.Color == 0);
        REQUIRE(tintEntry.Alpha == Approx(0.0f));
    }
}

TEST_CASE("v2.0.0 SkyMP load-order & RFAB modpack handshake verification", "[rfab][v200][loadorder]")
{
    const std::vector<std::string> serverMods{
        "Skyrim.esm",
        "Update.esm",
        "Dawnguard.esm",
        "HearthFires.esm",
        "Dragonborn.esm",
        "SkyrimTogether.esp",
        "RFAB.esp",
        "Requiem for the Indifferent.esp",
    };

    SECTION("Matching RFAB load order passes cleanly")
    {
        const auto clientMods = serverMods;
        const auto res = CoopPacketSafety::AuditModListCompatibility(serverMods, clientMods);
        REQUIRE(res.Compatible);
        REQUIRE(res.ServerHasRfab);
        REQUIRE(res.ClientHasRfab);
        REQUIRE(res.MissingCriticalMods.empty());
        REQUIRE(res.OrderMismatchCount == 0);
    }

    SECTION("Missing RFAB.esp on client is flagged with Russian diagnostic")
    {
        const std::vector<std::string> vanillaClient{
            "Skyrim.esm",
            "Update.esm",
            "Dawnguard.esm",
            "HearthFires.esm",
            "Dragonborn.esm",
            "SkyrimTogether.esp",
        };
        const auto res = CoopPacketSafety::AuditModListCompatibility(serverMods, vanillaClient);
        REQUIRE_FALSE(res.Compatible);
        REQUIRE(res.ServerHasRfab);
        REQUIRE_FALSE(res.ClientHasRfab);
        REQUIRE_FALSE(res.SummaryRu.empty());
    }

    SECTION("Inverted critical master order is detected")
    {
        const std::vector<std::string> invertedClient{
            "Skyrim.esm",
            "Update.esm",
            "Dragonborn.esm",
            "HearthFires.esm",
            "Dawnguard.esm",
            "SkyrimTogether.esp",
            "RFAB.esp",
            "Requiem for the Indifferent.esp",
        };
        const auto res = CoopPacketSafety::AuditModListCompatibility(serverMods, invertedClient);
        REQUIRE_FALSE(res.Compatible);
        REQUIRE(res.OrderMismatchCount >= 1);
    }
}
