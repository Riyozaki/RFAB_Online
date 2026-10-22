#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

#include <Components.h>
#include <Game/Player.h>
#include <GameServer.h>
#include <World.h>

#include <Events/PlayerEnterWorldEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>

#include <Gameplay/XpShare.h>
#include <Messages/NotifySyncRfabExperience.h>

#include <Services/ProgressionService.h>

namespace
{
Console::Setting bEnableProgressionLedger{"Gameplay:bEnableProgressionLedger",
                                          "Remember every player's RFAB experience and top returning players back up to it "
                                          "(protects against crashes and loading an older save)",
                                          true};

Console::Setting bXpBalanceNewcomers{"Gameplay:bXpBalanceNewcomers",
                                     "Start a player who has no ledger record at the average experience of the server "
                                     "(so a newcomer is not hours behind), instead of zero",
                                     true};

uint64_t NowUnixMs() noexcept
{
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}
} // namespace

ProgressionService::ProgressionService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_path = (std::filesystem::current_path() / "Data" / "progress" / "progress.jsonl").string();

    if (bEnableProgressionLedger.value_as<bool>())
        Load();

    // Restore on entering the world, not on connect: the award is applied on the
    // client through the RFAB global, which only exists once the player
    // character is loaded.
    m_enterWorldConnection = aDispatcher.sink<PlayerEnterWorldEvent>().connect<&ProgressionService::OnPlayerEnterWorld>(this);
    m_leaveConnection = aDispatcher.sink<PlayerLeaveEvent>().connect<&ProgressionService::OnPlayerLeave>(this);
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&ProgressionService::OnUpdate>(this);
}

ProgressionService::~ProgressionService() noexcept
{
    Flush(true);
}

void ProgressionService::Load() noexcept
{
    std::error_code ec;
    if (!std::filesystem::exists(m_path, ec))
    {
        spdlog::info("Progression: no ledger yet ({}), a new one will be created on the first save", m_path);
        return;
    }

    std::ifstream file(m_path, std::ios::binary);
    if (!file.is_open())
    {
        spdlog::warn("Progression: cannot read {}, progression restore is disabled for this run", m_path);
        return;
    }

    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const size_t count = m_ledger.Deserialize(text);

    spdlog::info("Progression: loaded {} player record(s) from {}", count, m_path);
}

void ProgressionService::OnAward(Player& aPlayer, uint32_t aAmount) noexcept
{
    if (!bEnableProgressionLedger.value_as<bool>() || aAmount == 0)
        return;

    // The server total tracks what the player has: their reported baseline plus
    // everything awarded to them since they connected.
    aPlayer.AddRfabXp(static_cast<int32_t>(std::min<uint32_t>(aAmount, 1'000'000)));

    Gameplay::Progression::PlayerRecord record = MakeRecord(aPlayer);
    m_ledger.Upsert(record);
    m_dirty = true;
}

void ProgressionService::OnPlayerEnterWorld(const PlayerEnterWorldEvent& acEvent) noexcept
{
    if (!bEnableProgressionLedger.value_as<bool>() || acEvent.pPlayer == nullptr)
        return;

    const Player& player = *acEvent.pPlayer;
    const auto* pRecord = m_ledger.Find(player.GetUsername().c_str());

    if (pRecord == nullptr)
    {
        // A player the server has never seen. Handing out zero means they start
        // hours behind everybody else in a session that has been running for a
        // while, which is exactly what "everyone progresses together" forbids.
        if (!bXpBalanceNewcomers.value_as<bool>() || m_ledger.Size() == 0)
            return;

        const uint32_t share = Gameplay::XpShare::AverageShare(m_ledger.TotalXp(), static_cast<uint32_t>(m_ledger.Size()));
        if (share == 0 || player.GetRfabXpTotal() >= static_cast<int32_t>(share))
            return;

        const uint32_t grant = share - static_cast<uint32_t>(std::max(0, player.GetRfabXpTotal()));

        NotifySyncRfabExperience notify{};
        notify.Experience = grant;
        player.Send(notify);

        if (Player* pMutable = m_world.GetPlayerManager().GetById(player.GetId()))
        {
            pMutable->SetRfabXpTotal(player.GetRfabXpTotal() + static_cast<int32_t>(grant));
            m_ledger.Upsert(MakeRecord(*pMutable));
            m_dirty = true;
        }

        spdlog::info("Progression: newcomer '{}' starts with the server average, granting {} XP", player.GetUsername().c_str(), grant);
        return;
    }

    const int32_t catchUp = Gameplay::Progression::ComputeCatchUp(pRecord->RfabXp, player.GetRfabXpTotal());
    if (catchUp <= 0)
    {
        spdlog::info("Progression: '{}' is at {} XP, nothing to restore", player.GetUsername().c_str(), player.GetRfabXpTotal());
        return;
    }

    NotifySyncRfabExperience notify{};
    notify.Experience = static_cast<uint32_t>(catchUp);
    player.Send(notify);

    // From this point the server considers the player topped up. The object is
    // const in this event, so the update goes through the manager.
    int32_t newTotal = player.GetRfabXpTotal() + catchUp;
    if (Player* pMutable = m_world.GetPlayerManager().GetById(player.GetId()))
    {
        pMutable->SetRfabXpTotal(player.GetRfabXpTotal() + catchUp);
        newTotal = pMutable->GetRfabXpTotal();
    }

    spdlog::info("Progression: restoring {} XP to '{}' ({} -> {})", catchUp, player.GetUsername().c_str(), player.GetRfabXpTotal(), newTotal);
}

void ProgressionService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    if (!bEnableProgressionLedger.value_as<bool>() || acEvent.pPlayer == nullptr)
        return;

    m_ledger.Upsert(MakeRecord(*acEvent.pPlayer));
    m_dirty = true;
    Flush(true);
}

void ProgressionService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!m_dirty)
        return;

    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastFlush < kFlushInterval)
        return;

    Flush();
}

Gameplay::Progression::PlayerRecord ProgressionService::MakeRecord(const Player& acPlayer) const noexcept
{
    Gameplay::Progression::PlayerRecord record;
    record.Username = acPlayer.GetUsername().c_str();
    record.RfabXp = acPlayer.GetRfabXpTotal();
    record.Level = acPlayer.GetLevel();
    record.UpdatedUnixMs = NowUnixMs();

    const auto& cell = acPlayer.GetCellComponent();
    record.CellBaseId = cell.Cell.BaseId;
    record.CellModId = cell.Cell.ModId;
    record.WorldSpaceBaseId = cell.WorldSpaceId.BaseId;
    record.WorldSpaceModId = cell.WorldSpaceId.ModId;

    // Position is diagnostic only; it comes from the character the server is
    // simulating, if the player has one.
    if (const auto character = acPlayer.GetCharacter(); character.has_value() && m_world.valid(*character))
    {
        if (const auto* pMovement = m_world.try_get<MovementComponent>(*character))
        {
            record.PositionX = pMovement->Position.x;
            record.PositionY = pMovement->Position.y;
            record.PositionZ = pMovement->Position.z;
        }
    }

    return record;
}

void ProgressionService::Flush(bool aForce) noexcept
{
    if (!bEnableProgressionLedger.value_as<bool>())
        return;

    if (!m_dirty && !aForce)
        return;

    m_lastFlush = std::chrono::steady_clock::now();

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(m_path).parent_path(), ec);
    if (ec)
    {
        spdlog::warn("Progression: cannot create {}: {}", m_path, ec.message());
        return;
    }

    // Write to a temporary file and swap it in, so a crash mid-write cannot
    // leave a half written ledger behind.
    const std::string tempPath = m_path + ".tmp";
    {
        std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            spdlog::warn("Progression: cannot write {}", tempPath);
            return;
        }

        file << m_ledger.Serialize();
        if (!file.good())
        {
            spdlog::warn("Progression: write to {} failed", tempPath);
            return;
        }
    }

    std::filesystem::remove(m_path, ec);
    std::filesystem::rename(tempPath, m_path, ec);
    if (ec)
    {
        spdlog::warn("Progression: cannot replace {}: {}", m_path, ec.message());
        return;
    }

    m_dirty = false;
}
