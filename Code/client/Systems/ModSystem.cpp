#include <TiltedOnlinePCH.h>

#include <Systems/ModSystem.h>

#include <Events/DisconnectedEvent.h>
#include <Structs/Mods.h>
#include <Structs/GameId.h>
#include <common/CoopPacketSafety.h>

#include <Games/TES.h>

#include <algorithm>
#include <mutex>

ModSystem::ModSystem(entt::dispatcher& aDispatcher) noexcept
{
    std::fill_n(m_standardToServer, 0x100, CoopPacketSafety::kUnmappedServerModId);
    // Deal with temporary ids
    m_standardToServer[0xFF] = CoopPacketSafety::kRuntimeDynamicModId;

    m_modsConnection = aDispatcher.sink<Mods>().connect<&ModSystem::HandleMods>(this);
    m_disconnectedConnection = aDispatcher.sink<DisconnectedEvent>().connect<&ModSystem::OnDisconnected>(this);
}

void ModSystem::OnDisconnected(const DisconnectedEvent&) noexcept
{
    std::unique_lock<std::shared_mutex> lock(m_mutex);
    m_liteToServer.clear();
    m_serverToGame.clear();
    std::fill_n(m_standardToServer, 0x100, CoopPacketSafety::kUnmappedServerModId);
    m_standardToServer[0xFF] = CoopPacketSafety::kRuntimeDynamicModId;
}

bool ModSystem::GetServerModId(const uint32_t aGameId, uint32_t& aModId, uint32_t& aBaseId) const noexcept
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    if (aGameId == 0u)
    {
        aModId = 0u;
        aBaseId = 0u;
        return true;
    }

    uint32_t hiByte = aGameId >> 24;

    // If it's a lite mod
    if (hiByte == 0xFE)
    {
        const uint16_t liteId = ((aGameId & 0x00FFF000) >> 12) & 0xFFFF;

        const auto itor = m_liteToServer.find(liteId);
        if (itor != std::end(m_liteToServer))
        {
            aModId = itor->second;
            aBaseId = aGameId & 0x00000FFF;
            return true;
        }

        aModId = 0u;
        aBaseId = 0u;
        return false;
    }

    // Here we have a standard or 0xFF runtime mod
    return CoopPacketSafety::TranslateStandardFormToServerModId(aGameId, m_standardToServer, aModId, aBaseId);
}

bool ModSystem::GetServerModId(uint32_t aGameId, GameId& aServerId) const noexcept
{
    return GetServerModId(aGameId, aServerId.ModId, aServerId.BaseId);
}

uint32_t ModSystem::GetGameId(uint32_t aServerId, uint32_t aFormId) const noexcept
{
    if (aServerId == 0u && aFormId == 0u)
        return 0u;

    std::shared_lock<std::shared_mutex> lock(m_mutex);

    auto itor = m_serverToGame.find(aServerId);
    if (itor != std::end(m_serverToGame))
    {
        if (itor->second.isLite)
        {
            aFormId &= 0xFFFu;
            aFormId |= 0xFE000000u;
            aFormId |= uint32_t(itor->second.id) << 12;
        }
        else
        {
            aFormId &= 0x00FFFFFFu;
            aFormId |= uint32_t(itor->second.id) << 24;
        }

        return aFormId;
    }

    return 0;
}

uint32_t ModSystem::GetGameId(const GameId& acGameId) const noexcept
{
    return GetGameId(acGameId.ModId, acGameId.BaseId);
}

void ModSystem::HandleMods(const Mods& acMods) noexcept
{
    Map<uint16_t, uint32_t> newLiteToServer;
    Map<uint32_t, GameMod> newServerToGame;
    uint32_t newStandardToServer[0x100];
    std::fill_n(newStandardToServer, 0x100, CoopPacketSafety::kUnmappedServerModId);
    newStandardToServer[0xFF] = CoopPacketSafety::kRuntimeDynamicModId;

    const auto pModManager = ModManager::Get();
    if (pModManager)
    {
        for (const auto& mod : acMods.ModList)
        {
            Mod* pMod = pModManager->GetByName(mod.Filename.c_str());
            if (pMod && pMod->IsLoaded())
            {
                if (pMod->IsLite())
                {
                    const uint16_t localLiteId = static_cast<uint16_t>(pMod->GetId() & 0xFFFu);
                    newServerToGame.emplace(mod.Id, GameMod{localLiteId, true});
                    newLiteToServer.emplace(localLiteId, mod.Id);
                }
                else
                {
                    const uint8_t localStdId = static_cast<uint8_t>(pMod->GetId() & 0xFFu);
                    if (localStdId < 0xFEu)
                    {
                        newServerToGame.emplace(mod.Id, GameMod{localStdId, false});
                        newStandardToServer[localStdId] = mod.Id;
                    }
                }
            }
            else
            {
                spdlog::error("Failed to find loaded mod {}, is lite? {}, id: {:X}", mod.Filename.c_str(), mod.IsLite, mod.Id);
            }
        }
    }

    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        m_liteToServer.swap(newLiteToServer);
        m_serverToGame.swap(newServerToGame);
        std::memcpy(m_standardToServer, newStandardToServer, sizeof(m_standardToServer));
    }
}
