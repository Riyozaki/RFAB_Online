#include <Services/SaveSnapshotService.h>

#include <common/CoopOwnership.h>
#include <common/CoopWorldView.h>
#include <common/RfabModpackCompat.h>
#include <Components.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>
#include <GameServer.h>
#include <Messages/CoordinatedSaveAck.h>
#include <Messages/NotifyActorTeleport.h>
#include <Messages/NotifyActorValueChanges.h>
#include <Messages/NotifyChatMessageBroadcast.h>
#include <Messages/NotifyCommitSave.h>
#include <Messages/NotifyDeathStateChange.h>
#include <Messages/NotifyExecuteLoad.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/NotifyPrepareSave.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/NotifyRfabProgressionEvent.h>
#include <Messages/RequestCoordinatedSaveLoad.h>
#include <Services/CalendarService.h>
#include <Services/CharacterService.h>
#include <Services/EncounterService.h>
#include <Services/PartyService.h>
#include <Setting.h>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>
#include <World.h>

#include <chrono>
#include <cstdio>
#include <random>

namespace
{
constexpr uint32_t kHealthAvIndex = 24;
constexpr uint32_t kMaxSerializedSnapshots = 16;

Console::Setting uPostCombatAutosaveInterval{
    "Gameplay:uPostCombatAutosaveInterval",
    "Minimum seconds between automatic post-combat checkpoints (boss kills always save immediately)",
    180U};

uint64_t NowSteadyMs() noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::string MakeSessionTag()
{
    std::random_device rd;
    const uint32_t val = rd() & 0xFFFFu;
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "S%04X", val);
    return std::string(buf);
}

void WriteStdString(TiltedPhoques::Buffer::Writer& aWriter, const std::string& acStr)
{
    TiltedPhoques::Serialization::WriteString(aWriter, TiltedPhoques::String(acStr.c_str(), acStr.size()));
}

std::string ReadStdString(TiltedPhoques::Buffer::Reader& aReader)
{
    const TiltedPhoques::String s = TiltedPhoques::Serialization::ReadString(aReader);
    return std::string(s.c_str(), s.size());
}

void SerializeProgressionSnapshot(TiltedPhoques::Buffer::Writer& aWriter, const RfabProgressionLedger::Snapshot& acProg)
{
    using TiltedPhoques::Serialization;
    Serialization::WriteVarInt(aWriter, static_cast<uint64_t>(acProg.TotalSharedXp));

    Serialization::WriteVarInt(aWriter, acProg.GrantedXpBySource.size());
    for (const auto& [src, xp] : acProg.GrantedXpBySource)
    {
        WriteStdString(aWriter, src);
        aWriter.WriteBits(static_cast<uint64_t>(static_cast<uint32_t>(xp)), 32);
    }

    Serialization::WriteVarInt(aWriter, acProg.ReadBooks.size());
    for (const auto& book : acProg.ReadBooks)
        book.Serialize(aWriter);

    Serialization::WriteVarInt(aWriter, acProg.SyncedGlobals.size());
    for (const auto& [edid, val] : acProg.SyncedGlobals)
    {
        WriteStdString(aWriter, edid);
        Serialization::WriteFloat(aWriter, val);
    }

    Serialization::WriteVarInt(aWriter, acProg.BossGhostStates.size());
    for (const auto& [id, ghost] : acProg.BossGhostStates)
    {
        Serialization::WriteVarInt(aWriter, id);
        Serialization::WriteBool(aWriter, ghost);
    }
}

void DeserializeProgressionSnapshot(TiltedPhoques::Buffer::Reader& aReader, RfabProgressionLedger::Snapshot& aOutProg)
{
    using TiltedPhoques::Serialization;
    aOutProg = {};
    aOutProg.TotalSharedXp = static_cast<int64_t>(Serialization::ReadVarInt(aReader));

    const uint64_t srcCount = std::min<uint64_t>(Serialization::ReadVarInt(aReader), 65536ULL);
    for (uint64_t i = 0; i < srcCount; ++i)
    {
        const std::string src = ReadStdString(aReader);
        uint64_t rawXp = 0;
        aReader.ReadBits(rawXp, 32);
        aOutProg.GrantedXpBySource[src] = static_cast<int32_t>(static_cast<uint32_t>(rawXp));
    }

    const uint64_t bookCount = std::min<uint64_t>(Serialization::ReadVarInt(aReader), 65536ULL);
    aOutProg.ReadBooks.reserve(static_cast<size_t>(bookCount));
    for (uint64_t i = 0; i < bookCount; ++i)
    {
        GameId book{};
        book.Deserialize(aReader);
        aOutProg.ReadBooks.push_back(book);
    }

    const uint64_t globalCount = std::min<uint64_t>(Serialization::ReadVarInt(aReader), 65536ULL);
    for (uint64_t i = 0; i < globalCount; ++i)
    {
        const std::string edid = ReadStdString(aReader);
        const float val = Serialization::ReadFloat(aReader);
        aOutProg.SyncedGlobals[edid] = val;
    }

    const uint64_t ghostCount = std::min<uint64_t>(Serialization::ReadVarInt(aReader), 65536ULL);
    for (uint64_t i = 0; i < ghostCount; ++i)
    {
        const uint32_t id = static_cast<uint32_t>(Serialization::ReadVarInt(aReader));
        const bool ghost = Serialization::ReadBool(aReader);
        aOutProg.BossGhostStates[id] = ghost;
    }
}

void SerializePlayerStateRecord(TiltedPhoques::Buffer::Writer& aWriter, const SaveSnapshotService::PlayerStateRecord& acRec)
{
    using TiltedPhoques::Serialization;
    Serialization::WriteBool(aWriter, acRec.HasValidTransform);
    if (acRec.HasValidTransform)
    {
        acRec.CellId.Serialize(aWriter);
        acRec.WorldSpaceId.Serialize(aWriter);
        acRec.CenterCoords.Serialize(aWriter);
        Serialization::WriteFloat(aWriter, acRec.Position.x);
        Serialization::WriteFloat(aWriter, acRec.Position.y);
        Serialization::WriteFloat(aWriter, acRec.Position.z);
        Serialization::WriteFloat(aWriter, acRec.Rotation.x);
        Serialization::WriteFloat(aWriter, acRec.Rotation.y);
        Serialization::WriteFloat(aWriter, acRec.Rotation.z);
    }
    Serialization::WriteBool(aWriter, acRec.HasInventory);
    if (acRec.HasInventory)
        acRec.CurrentInventory.Serialize(aWriter);
}

void DeserializePlayerStateRecord(TiltedPhoques::Buffer::Reader& aReader, SaveSnapshotService::PlayerStateRecord& aOutRec)
{
    using TiltedPhoques::Serialization;
    aOutRec = {};
    aOutRec.HasValidTransform = Serialization::ReadBool(aReader);
    if (aOutRec.HasValidTransform)
    {
        aOutRec.CellId.Deserialize(aReader);
        aOutRec.WorldSpaceId.Deserialize(aReader);
        aOutRec.CenterCoords.Deserialize(aReader);
        aOutRec.Position.x = Serialization::ReadFloat(aReader);
        aOutRec.Position.y = Serialization::ReadFloat(aReader);
        aOutRec.Position.z = Serialization::ReadFloat(aReader);
        aOutRec.Rotation.x = Serialization::ReadFloat(aReader);
        aOutRec.Rotation.y = Serialization::ReadFloat(aReader);
        aOutRec.Rotation.z = Serialization::ReadFloat(aReader);
    }
    aOutRec.HasInventory = Serialization::ReadBool(aReader);
    if (aOutRec.HasInventory)
        aOutRec.CurrentInventory.Deserialize(aReader);
}

std::string MakePlayerIdentityKey(const Player* apPlayer)
{
    if (!apPlayer)
        return {};
    const auto& username = apPlayer->GetUsername();
    return CoopWorldView::BuildPlayerIdentityKey(
        apPlayer->GetDiscordId(),
        std::string_view(username.data(), username.size()),
        apPlayer->GetId());
}
} // namespace

std::filesystem::path SaveSnapshotService::GetDefaultCheckpointStorePath()
{
    return std::filesystem::current_path() / "Data" / "rfab_checkpoints.bin";
}

SaveSnapshotService::SaveSnapshotService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
    , m_coordinator(MakeSessionTag(), SaveSnapshotCoordinator::kDefaultRingSize)
    , m_updateConnection(aDispatcher.sink<UpdateEvent>().connect<&SaveSnapshotService::OnUpdate>(this))
    , m_playerLeaveConnection(aDispatcher.sink<PlayerLeaveEvent>().connect<&SaveSnapshotService::OnPlayerLeave>(this))
    , m_requestConnection(aDispatcher.sink<PacketEvent<RequestCoordinatedSaveLoad>>().connect<&SaveSnapshotService::OnSaveLoadRequest>(this))
    , m_ackConnection(aDispatcher.sink<PacketEvent<CoordinatedSaveAck>>().connect<&SaveSnapshotService::OnSaveAck>(this))
{
    if (LoadCheckpointsFromDisk(GetDefaultCheckpointStorePath()))
    {
        spdlog::info(
            "[RFAB SaveSnapshot] Restored {} persisted checkpoint(s) from disk (SessionTag={}, LastEpoch={}, LastSlot='{}')",
            m_snapshots.size(),
            m_coordinator.GetSessionTag(),
            m_coordinator.GetLastCommittedEpoch(),
            m_coordinator.GetLastCommittedSlot());
    }
}

bool SaveSnapshotService::SaveCheckpointsToDisk(const std::filesystem::path& acPath) const noexcept
{
    using TiltedPhoques::Serialization;
    try
    {
        TiltedPhoques::Buffer rawBuffer(4 * 1024 * 1024);
        TiltedPhoques::Buffer::Writer writer(&rawBuffer);

        WriteStdString(writer, m_coordinator.GetSessionTag());
        Serialization::WriteVarInt(writer, m_coordinator.GetNextEpochId());
        Serialization::WriteVarInt(writer, m_coordinator.GetCurrentWorldEpoch());
        Serialization::WriteVarInt(writer, m_coordinator.GetManualRingCursor());
        Serialization::WriteVarInt(writer, m_coordinator.GetAutoRingCursor());
        Serialization::WriteVarInt(writer, m_coordinator.GetLastCommittedEpoch());
        WriteStdString(writer, m_coordinator.GetLastCommittedSlot());

        Serialization::WriteVarInt(writer, m_snapshots.size());
        for (const auto& snap : m_snapshots)
        {
            Serialization::WriteVarInt(writer, snap.EpochId);
            WriteStdString(writer, snap.SlotName);
            Serialization::WriteBool(writer, snap.IsAutoCheckpoint);
            Serialization::WriteVarInt(writer, snap.CreatedAtMs);

            Serialization::WriteVarInt(writer, snap.QuestLogsByPlayerId.size());
            for (const auto& [pid, qlog] : snap.QuestLogsByPlayerId)
            {
                Serialization::WriteVarInt(writer, pid);
                qlog.Serialize(writer);
            }
            snap.SharedPartyQuestLog.Serialize(writer);

            Serialization::WriteVarInt(writer, snap.PlayersById.size());
            for (const auto& [pid, pRec] : snap.PlayersById)
            {
                Serialization::WriteVarInt(writer, pid);
                SerializePlayerStateRecord(writer, pRec);
            }

            Serialization::WriteVarInt(writer, snap.TrackedActors.size());
            for (const auto& [entId, aRec] : snap.TrackedActors)
            {
                Serialization::WriteVarInt(writer, entId);
                Serialization::WriteFloat(writer, aRec.CurrentHealth);
                Serialization::WriteFloat(writer, aRec.MaxHealth);
                Serialization::WriteBool(writer, aRec.IsDead);
            }

            Serialization::WriteVarInt(writer, snap.TrackedObjects.size());
            for (const auto& objRec : snap.TrackedObjects)
            {
                objRec.Id.Serialize(writer);
                objRec.CellId.Serialize(writer);
                objRec.WorldSpaceId.Serialize(writer);
                objRec.CenterCoords.Serialize(writer);
                objRec.CurrentLockData.Serialize(writer);
                objRec.CurrentInventory.Serialize(writer);
            }

            SerializeProgressionSnapshot(writer, snap.Progression);

            Serialization::WriteVarInt(writer, static_cast<uint64_t>(static_cast<uint32_t>(snap.Calendar.Hour)));
            Serialization::WriteVarInt(writer, static_cast<uint64_t>(static_cast<uint32_t>(snap.Calendar.Minute)));
            Serialization::WriteFloat(writer, snap.Calendar.TimeScale);
            Serialization::WriteVarInt(writer, static_cast<uint64_t>(static_cast<uint32_t>(snap.Calendar.Day)));
            Serialization::WriteVarInt(writer, static_cast<uint64_t>(static_cast<uint32_t>(snap.Calendar.Month)));
            Serialization::WriteFloat(writer, snap.Calendar.Year);
        }

        // Trailing v2.1+ NPC spatial extension block (100% backwards-compatible with v2.0 checkpoint files!)
        constexpr uint64_t kNpcSpatialExtMagic = 0x53504154323031ULL;
        Serialization::WriteVarInt(writer, kNpcSpatialExtMagic);
        Serialization::WriteVarInt(writer, m_snapshots.size());
        for (const auto& snap : m_snapshots)
        {
            size_t spatialCount = 0;
            for (const auto& [_, aRec] : snap.TrackedActors)
            {
                if (aRec.HasSpatialState)
                    ++spatialCount;
            }
            Serialization::WriteVarInt(writer, spatialCount);
            for (const auto& [entId, aRec] : snap.TrackedActors)
            {
                if (!aRec.HasSpatialState)
                    continue;
                Serialization::WriteVarInt(writer, entId);
                aRec.CellId.Serialize(writer);
                aRec.WorldSpaceId.Serialize(writer);
                aRec.CenterCoords.Serialize(writer);
                Serialization::WriteFloat(writer, aRec.Position.x);
                Serialization::WriteFloat(writer, aRec.Position.y);
                Serialization::WriteFloat(writer, aRec.Position.z);
                Serialization::WriteFloat(writer, aRec.Rotation.x);
                Serialization::WriteFloat(writer, aRec.Rotation.y);
                Serialization::WriteFloat(writer, aRec.Rotation.z);
            }
        }

        // Trailing v2.0.3+ reconnect-stable player identity + ContainerLootLedger snapshot block
        // (100% backward-compatible with v2.0.1 and v2.0.2 checkpoint files!)
        constexpr uint64_t kV203IdentityAndLedgerExtMagic = 0x49444C47323033ULL;
        Serialization::WriteVarInt(writer, kV203IdentityAndLedgerExtMagic);
        Serialization::WriteVarInt(writer, m_snapshots.size());
        for (const auto& snap : m_snapshots)
        {
            Serialization::WriteVarInt(writer, snap.QuestLogsByIdentityKey.size());
            for (const auto& [identKey, qlog] : snap.QuestLogsByIdentityKey)
            {
                WriteStdString(writer, identKey);
                qlog.Serialize(writer);
            }

            Serialization::WriteVarInt(writer, snap.PlayersByIdentityKey.size());
            for (const auto& [identKey, pRec] : snap.PlayersByIdentityKey)
            {
                WriteStdString(writer, identKey);
                SerializePlayerStateRecord(writer, pRec);
            }

            Serialization::WriteVarInt(writer, static_cast<uint64_t>(snap.LootLedger.size()));
            for (const auto& entry : snap.LootLedger)
            {
                Serialization::WriteVarInt(writer, entry.ContainerServerId);
                Serialization::WriteVarInt(writer, entry.ModId);
                Serialization::WriteVarInt(writer, entry.BaseId);
                Serialization::WriteVarInt(writer, entry.ClaimantPlayerId);
                Serialization::WriteVarInt(writer, entry.DepletedAtMs);
            }
        }

        // Trailing v2.1.0+ self-describing TLV extension container (`kTlvContainerMagic`)
        // holding persistent placed actor snapshots (`PersistentActorsByKey`) keyed by FormId.
        {
            constexpr uint64_t kV210TlvContainerExtMagic = 0x544C5643323130ULL;
            TiltedPhoques::Buffer tlvChunkBuf(512 * 1024);
            TiltedPhoques::Buffer::Writer chunkWriter(&tlvChunkBuf);
            Serialization::WriteVarInt(chunkWriter, m_snapshots.size());
            for (const auto& snap : m_snapshots)
            {
                Serialization::WriteVarInt(chunkWriter, snap.PersistentActorsByKey.size());
                for (const auto& [packedKey, aRec] : snap.PersistentActorsByKey)
                {
                    Serialization::WriteVarInt(chunkWriter, packedKey);
                    aRec.FormId.Serialize(chunkWriter);
                    aRec.BaseId.Serialize(chunkWriter);
                    Serialization::WriteFloat(chunkWriter, aRec.CurrentHealth);
                    Serialization::WriteFloat(chunkWriter, aRec.MaxHealth);
                    Serialization::WriteBool(chunkWriter, aRec.IsDead);
                    Serialization::WriteBool(chunkWriter, aRec.HasSpatialState);
                    if (aRec.HasSpatialState)
                    {
                        aRec.CellId.Serialize(chunkWriter);
                        aRec.WorldSpaceId.Serialize(chunkWriter);
                        aRec.CenterCoords.Serialize(chunkWriter);
                        Serialization::WriteFloat(chunkWriter, aRec.Position.x);
                        Serialization::WriteFloat(chunkWriter, aRec.Position.y);
                        Serialization::WriteFloat(chunkWriter, aRec.Position.z);
                        Serialization::WriteFloat(chunkWriter, aRec.Rotation.x);
                        Serialization::WriteFloat(chunkWriter, aRec.Rotation.y);
                        Serialization::WriteFloat(chunkWriter, aRec.Rotation.z);
                    }
                }
            }

            CoopCheckpoint::TlvContainerBuilder tlvBuilder;
            tlvBuilder.AppendChunk(
                CoopCheckpoint::TlvTag::kV210PersistentActors,
                tlvChunkBuf.GetWriteData(),
                chunkWriter.Size());
            const std::vector<uint8_t> tlvBytes = tlvBuilder.Bytes();
            if (!tlvBytes.empty())
            {
                Serialization::WriteVarInt(writer, kV210TlvContainerExtMagic);
                Serialization::WriteVarInt(writer, static_cast<uint64_t>(tlvBytes.size()));
                for (const uint8_t byteVal : tlvBytes)
                {
                    writer.WriteBits(static_cast<uint64_t>(byteVal), 8);
                }
            }
        }

        return CoopCheckpoint::WriteAtomicCheckpointFile(acPath, rawBuffer.GetWriteData(), writer.Size());
    }
    catch (...)
    {
        return false;
    }
}

bool SaveSnapshotService::LoadCheckpointsFromDisk(const std::filesystem::path& acPath) noexcept
{
    using TiltedPhoques::Serialization;
    std::vector<uint8_t> payload;
    if (!CoopCheckpoint::ReadValidatedCheckpointFile(acPath, payload) || payload.empty())
        return false;

    try
    {
        TiltedPhoques::Buffer rawBuffer(payload.size());
        std::memcpy(rawBuffer.GetWriteData(), payload.data(), payload.size());
        TiltedPhoques::Buffer::Reader reader(&rawBuffer);

        const std::string sessionTag = ReadStdString(reader);
        const uint64_t nextEpochId = Serialization::ReadVarInt(reader);
        const uint64_t worldGen = Serialization::ReadVarInt(reader);
        const size_t manualCursor = static_cast<size_t>(Serialization::ReadVarInt(reader));
        const size_t autoCursor = static_cast<size_t>(Serialization::ReadVarInt(reader));
        const uint64_t lastCommittedEpoch = Serialization::ReadVarInt(reader);
        const std::string lastCommittedSlot = ReadStdString(reader);

        const uint64_t snapCount = std::min<uint64_t>(Serialization::ReadVarInt(reader), kMaxSerializedSnapshots);
        std::deque<WorldSnapshot> loadedSnapshots;
        for (uint64_t sIdx = 0; sIdx < snapCount; ++sIdx)
        {
            WorldSnapshot snap{};
            snap.EpochId = Serialization::ReadVarInt(reader);
            snap.SlotName = ReadStdString(reader);
            snap.IsAutoCheckpoint = Serialization::ReadBool(reader);
            snap.CreatedAtMs = Serialization::ReadVarInt(reader);

            const uint64_t qCount = std::min<uint64_t>(Serialization::ReadVarInt(reader), 256ULL);
            for (uint64_t i = 0; i < qCount; ++i)
            {
                const uint32_t pid = static_cast<uint32_t>(Serialization::ReadVarInt(reader));
                QuestLog qlog{};
                qlog.Deserialize(reader);
                snap.QuestLogsByPlayerId[pid] = std::move(qlog);
            }
            snap.SharedPartyQuestLog.Deserialize(reader);

            const uint64_t pCount = std::min<uint64_t>(Serialization::ReadVarInt(reader), 256ULL);
            for (uint64_t i = 0; i < pCount; ++i)
            {
                const uint32_t pid = static_cast<uint32_t>(Serialization::ReadVarInt(reader));
                PlayerStateRecord pRec{};
                DeserializePlayerStateRecord(reader, pRec);
                snap.PlayersById[pid] = std::move(pRec);
            }

            const uint64_t aCount = std::min<uint64_t>(Serialization::ReadVarInt(reader), 65536ULL);
            for (uint64_t i = 0; i < aCount; ++i)
            {
                const uint32_t entId = static_cast<uint32_t>(Serialization::ReadVarInt(reader));
                ActorStateRecord aRec{};
                aRec.CurrentHealth = Serialization::ReadFloat(reader);
                aRec.MaxHealth = Serialization::ReadFloat(reader);
                aRec.IsDead = Serialization::ReadBool(reader);
                snap.TrackedActors[entId] = aRec;
            }

            const uint64_t oCount = std::min<uint64_t>(Serialization::ReadVarInt(reader), 65536ULL);
            snap.TrackedObjects.reserve(static_cast<size_t>(oCount));
            for (uint64_t i = 0; i < oCount; ++i)
            {
                ObjectStateRecord objRec{};
                objRec.Id.Deserialize(reader);
                objRec.CellId.Deserialize(reader);
                objRec.WorldSpaceId.Deserialize(reader);
                objRec.CenterCoords.Deserialize(reader);
                objRec.CurrentLockData.Deserialize(reader);
                objRec.CurrentInventory.Deserialize(reader);
                snap.TrackedObjects.push_back(std::move(objRec));
            }

            DeserializeProgressionSnapshot(reader, snap.Progression);

            snap.Calendar.Hour = static_cast<int>(static_cast<uint32_t>(Serialization::ReadVarInt(reader)));
            snap.Calendar.Minute = static_cast<int>(static_cast<uint32_t>(Serialization::ReadVarInt(reader)));
            snap.Calendar.TimeScale = Serialization::ReadFloat(reader);
            snap.Calendar.Day = static_cast<int>(static_cast<uint32_t>(Serialization::ReadVarInt(reader)));
            snap.Calendar.Month = static_cast<int>(static_cast<uint32_t>(Serialization::ReadVarInt(reader)));
            snap.Calendar.Year = Serialization::ReadFloat(reader);

            loadedSnapshots.push_back(std::move(snap));
        }

        if (!reader.Eof())
        {
            constexpr uint64_t kNpcSpatialExtMagic = 0x53504154323031ULL;
            const uint64_t extMagic = Serialization::ReadVarInt(reader);
            if (extMagic == kNpcSpatialExtMagic)
            {
                const uint64_t extSnapCount = std::min<uint64_t>(Serialization::ReadVarInt(reader), loadedSnapshots.size());
                for (uint64_t sIdx = 0; sIdx < extSnapCount; ++sIdx)
                {
                    auto& snap = loadedSnapshots[static_cast<size_t>(sIdx)];
                    const uint64_t spatialCount = std::min<uint64_t>(Serialization::ReadVarInt(reader), 65536ULL);
                    for (uint64_t i = 0; i < spatialCount; ++i)
                    {
                        const uint32_t entId = static_cast<uint32_t>(Serialization::ReadVarInt(reader));
                        GameId cellId{};
                        GameId wsId{};
                        GridCellCoords coords{};
                        cellId.Deserialize(reader);
                        wsId.Deserialize(reader);
                        coords.Deserialize(reader);
                        glm::vec3 pos{};
                        glm::vec3 rot{};
                        pos.x = Serialization::ReadFloat(reader);
                        pos.y = Serialization::ReadFloat(reader);
                        pos.z = Serialization::ReadFloat(reader);
                        rot.x = Serialization::ReadFloat(reader);
                        rot.y = Serialization::ReadFloat(reader);
                        rot.z = Serialization::ReadFloat(reader);

                        auto actorIt = snap.TrackedActors.find(entId);
                        if (actorIt != snap.TrackedActors.end())
                        {
                            actorIt->second.HasSpatialState = true;
                            actorIt->second.CellId = cellId;
                            actorIt->second.WorldSpaceId = wsId;
                            actorIt->second.CenterCoords = coords;
                            actorIt->second.Position = pos;
                            actorIt->second.Rotation = rot;
                        }
                    }
                }
            }

            if (!reader.Eof())
            {
                constexpr uint64_t kV203IdentityAndLedgerExtMagic = 0x49444C47323033ULL;
                const uint64_t extMagic2 = Serialization::ReadVarInt(reader);
                if (extMagic2 == kV203IdentityAndLedgerExtMagic)
                {
                    const uint64_t extSnapCount2 = std::min<uint64_t>(Serialization::ReadVarInt(reader), loadedSnapshots.size());
                    for (uint64_t sIdx = 0; sIdx < extSnapCount2; ++sIdx)
                    {
                        auto& snap = loadedSnapshots[static_cast<size_t>(sIdx)];

                        const uint64_t qIdentCount = std::min<uint64_t>(Serialization::ReadVarInt(reader), 256ULL);
                        for (uint64_t i = 0; i < qIdentCount; ++i)
                        {
                            const std::string identKey = ReadStdString(reader);
                            QuestLog qlog{};
                            qlog.Deserialize(reader);
                            if (!identKey.empty())
                                snap.QuestLogsByIdentityKey[identKey] = std::move(qlog);
                        }

                        const uint64_t pIdentCount = std::min<uint64_t>(Serialization::ReadVarInt(reader), 256ULL);
                        for (uint64_t i = 0; i < pIdentCount; ++i)
                        {
                            const std::string identKey = ReadStdString(reader);
                            PlayerStateRecord pRec{};
                            DeserializePlayerStateRecord(reader, pRec);
                            if (!identKey.empty())
                                snap.PlayersByIdentityKey[identKey] = std::move(pRec);
                        }

                        const uint64_t ledgerCount = std::min<uint64_t>(
                            Serialization::ReadVarInt(reader),
                            static_cast<uint64_t>(CoopLootLedger::ContainerLootLedger::kMaxTrackedDepletions));
                        snap.LootLedger.reserve(static_cast<size_t>(ledgerCount));
                        for (uint64_t i = 0; i < ledgerCount; ++i)
                        {
                            CoopLootLedger::ContainerLootLedger::SnapshotEntry entry{};
                            entry.ContainerServerId = static_cast<uint32_t>(Serialization::ReadVarInt(reader));
                            entry.ModId = static_cast<uint32_t>(Serialization::ReadVarInt(reader));
                            entry.BaseId = static_cast<uint32_t>(Serialization::ReadVarInt(reader));
                            entry.ClaimantPlayerId = static_cast<uint32_t>(Serialization::ReadVarInt(reader));
                            entry.DepletedAtMs = Serialization::ReadVarInt(reader);
                            snap.LootLedger.push_back(entry);
                        }
                    }
                }

                if (!reader.Eof())
                {
                    constexpr uint64_t kV210TlvContainerExtMagic = 0x544C5643323130ULL;
                    const uint64_t extMagic3 = Serialization::ReadVarInt(reader);
                    if (extMagic3 == kV210TlvContainerExtMagic)
                    {
                        const uint64_t tlvByteCount = std::min<uint64_t>(
                            Serialization::ReadVarInt(reader),
                            4ULL * 1024ULL * 1024ULL);
                        std::vector<uint8_t> tlvBlob(static_cast<size_t>(tlvByteCount), 0u);
                        for (size_t bIdx = 0; bIdx < tlvBlob.size(); ++bIdx)
                        {
                            uint64_t rawByte = 0;
                            reader.ReadBits(rawByte, 8);
                            tlvBlob[bIdx] = static_cast<uint8_t>(rawByte & 0xFFu);
                        }

                        std::vector<CoopCheckpoint::TlvChunkView> tlvChunks;
                        if (!tlvBlob.empty() &&
                            CoopCheckpoint::ParseTlvContainer(
                                tlvBlob.data(),
                                tlvBlob.size(),
                                tlvChunks))
                        {
                            for (const auto& chunk : tlvChunks)
                            {
                                if (chunk.Tag != static_cast<uint32_t>(CoopCheckpoint::TlvTag::kV210PersistentActors) ||
                                    !chunk.Data || chunk.ByteLength == 0)
                                {
                                    continue;
                                }

                                TiltedPhoques::Buffer chunkBuffer(chunk.ByteLength);
                                std::memcpy(chunkBuffer.GetWriteData(), chunk.Data, chunk.ByteLength);
                                TiltedPhoques::Buffer::Reader chunkReader(&chunkBuffer);

                                const uint64_t tlvSnapCount = std::min<uint64_t>(
                                    Serialization::ReadVarInt(chunkReader),
                                    loadedSnapshots.size());
                                for (uint64_t sIdx = 0; sIdx < tlvSnapCount; ++sIdx)
                                {
                                    auto& snap = loadedSnapshots[static_cast<size_t>(sIdx)];
                                    const uint64_t actorCount = std::min<uint64_t>(
                                        Serialization::ReadVarInt(chunkReader),
                                        65536ULL);
                                    for (uint64_t i = 0; i < actorCount; ++i)
                                    {
                                        const uint64_t packedKey = Serialization::ReadVarInt(chunkReader);
                                        ActorStateRecord aRec{};
                                        aRec.FormId.Deserialize(chunkReader);
                                        aRec.BaseId.Deserialize(chunkReader);
                                        aRec.CurrentHealth = Serialization::ReadFloat(chunkReader);
                                        aRec.MaxHealth = Serialization::ReadFloat(chunkReader);
                                        aRec.IsDead = Serialization::ReadBool(chunkReader);
                                        aRec.HasSpatialState = Serialization::ReadBool(chunkReader);
                                        if (aRec.HasSpatialState)
                                        {
                                            aRec.CellId.Deserialize(chunkReader);
                                            aRec.WorldSpaceId.Deserialize(chunkReader);
                                            aRec.CenterCoords.Deserialize(chunkReader);
                                            aRec.Position.x = Serialization::ReadFloat(chunkReader);
                                            aRec.Position.y = Serialization::ReadFloat(chunkReader);
                                            aRec.Position.z = Serialization::ReadFloat(chunkReader);
                                            aRec.Rotation.x = Serialization::ReadFloat(chunkReader);
                                            aRec.Rotation.y = Serialization::ReadFloat(chunkReader);
                                            aRec.Rotation.z = Serialization::ReadFloat(chunkReader);
                                        }
                                        if (packedKey != 0)
                                            snap.PersistentActorsByKey[packedKey] = aRec;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        m_coordinator.RestorePersistedState(
            sessionTag,
            nextEpochId,
            worldGen,
            manualCursor,
            autoCursor,
            lastCommittedEpoch,
            lastCommittedSlot);
        m_snapshots = std::move(loadedSnapshots);
        HydratePersistedWorldState();

        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool SaveSnapshotService::IsPlayerAuthorizedHost(Player* apPlayer) noexcept
{
    if (!apPlayer)
        return false;

    if (auto* pServer = GameServer::Get())
    {
        if (pServer->GetAdminSessions().contains(apPlayer->GetConnectionId()))
            return true;
    }

    auto& partyService = m_world.GetPartyService();
    if (partyService.IsPlayerInParty(apPlayer))
        return partyService.IsPlayerLeader(apPlayer);

    Player* pFirstPlayer = nullptr;
    for (Player* pCandidate : m_world.GetPlayerManager())
    {
        if (!pFirstPlayer || pCandidate->GetId() < pFirstPlayer->GetId())
            pFirstPlayer = pCandidate;
    }

    return pFirstPlayer == apPlayer;
}

std::optional<SaveSnapshotService::PlayerStateRecord> SaveSnapshotService::CapturePlayerState(Player* apPlayer) const noexcept
{
    if (!apPlayer)
        return std::nullopt;

    PlayerStateRecord rec{};
    const auto& cellComp = apPlayer->GetCellComponent();
    if (cellComp.Cell)
    {
        rec.CellId = cellComp.Cell;
        rec.WorldSpaceId = cellComp.WorldSpaceId;
        rec.CenterCoords = cellComp.CenterCoords;
    }

    if (apPlayer->GetCharacter().has_value())
    {
        const entt::entity charEntity = *apPlayer->GetCharacter();
        if (m_world.valid(charEntity))
        {
            if (const auto* pCharCell = m_world.try_get<CellIdComponent>(charEntity); pCharCell && pCharCell->Cell)
            {
                rec.CellId = pCharCell->Cell;
                rec.WorldSpaceId = pCharCell->WorldSpaceId;
                rec.CenterCoords = pCharCell->CenterCoords;
            }
            if (const auto* pMov = m_world.try_get<MovementComponent>(charEntity))
            {
                if (CoopOwnership::IsValidHandoffCoordinates(pMov->Position.x, pMov->Position.y, pMov->Position.z) && rec.CellId)
                {
                    rec.Position = pMov->Position;
                    rec.Rotation = pMov->Rotation;
                    rec.HasValidTransform = true;
                }
            }
            if (const auto* pInv = m_world.try_get<InventoryComponent>(charEntity))
            {
                rec.CurrentInventory = pInv->Content;
                rec.HasInventory = true;
            }
        }
    }

    if (!rec.HasValidTransform && !rec.HasInventory)
        return std::nullopt;

    return rec;
}

void SaveSnapshotService::HydratePersistedWorldState() noexcept
{
    if (m_snapshots.empty())
        return;

    const auto& latest = m_snapshots.back();
    if (auto* pProg = m_world.ctx().find<RfabProgressionService>())
        pProg->GetLedger().RestoreSnapshot(latest.Progression);

    if (auto* pInv = m_world.ctx().find<InventoryService>())
        pInv->RestoreLootLedgerSnapshot(latest.LootLedger);

    if (auto* pObj = m_world.ctx().find<ObjectService>())
    {
        for (const auto& objRec : latest.TrackedObjects)
        {
            pObj->UpsertRestoredObject(
                objRec.Id,
                objRec.CellId,
                objRec.WorldSpaceId,
                objRec.CenterCoords,
                objRec.CurrentLockData,
                objRec.CurrentInventory);
        }
    }

    if (auto* pCharSvc = m_world.ctx().find<CharacterService>())
    {
        for (const auto& [_, aRec] : latest.PersistentActorsByKey)
        {
            if (!aRec.FormId || aRec.FormId.ModId == 0xFFFFFFFFu || !aRec.HasSpatialState)
                continue;
            (void)pCharSvc->EnsureDormantPersistentCharacter(
                aRec.FormId,
                aRec.BaseId,
                aRec.CellId,
                aRec.WorldSpaceId,
                aRec.CenterCoords,
                aRec.Position,
                aRec.Rotation,
                aRec.IsDead,
                aRec.CurrentHealth,
                aRec.MaxHealth);
        }
    }
}

void SaveSnapshotService::RefreshPlayerJoinBaselines() noexcept
{
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (!pPlayer)
            continue;
        auto it = m_playerJoinBaselines.find(pPlayer->GetId());
        if (it != m_playerJoinBaselines.end() && it->second.HasValidTransform)
            continue;

        if (auto captured = CapturePlayerState(pPlayer); captured.has_value() && captured->HasValidTransform)
        {
            const std::string identKey = MakePlayerIdentityKey(pPlayer);
            if (!identKey.empty() && !m_playerJoinBaselinesByIdentityKey.contains(identKey))
                m_playerJoinBaselinesByIdentityKey[identKey] = *captured;
            m_playerJoinBaselines[pPlayer->GetId()] = std::move(*captured);
        }
    }
}

bool SaveSnapshotService::TriggerCoordinatedSave(bool aIsAutoCheckpoint, Player* apRequester) noexcept
{
    if (apRequester && !IsPlayerAuthorizedHost(apRequester))
        return false;

    std::vector<uint32_t> playerIds;
    for (Player* pPlayer : m_world.GetPlayerManager())
        playerIds.push_back(pPlayer->GetId());

    const uint64_t nowMs = NowSteadyMs();
    const auto barrierOpt = m_coordinator.BeginBarrier(playerIds, aIsAutoCheckpoint, nowMs);
    if (!barrierOpt.has_value())
        return false;

    NotifyPrepareSave prepare{};
    prepare.EpochId = barrierOpt->EpochId;
    prepare.SlotName = String(barrierOpt->SlotName.c_str());
    prepare.IsAutoCheckpoint = aIsAutoCheckpoint;

    if (auto* pServer = GameServer::Get())
        pServer->SendToPlayers(prepare, nullptr);
    spdlog::info(
        "[RFAB SaveSnapshot] Started two-phase save barrier Epoch={} Slot='{}' Auto={} Players={}",
        barrierOpt->EpochId,
        barrierOpt->SlotName,
        aIsAutoCheckpoint,
        playerIds.size());
    return true;
}

SaveSnapshotService::WorldSnapshot SaveSnapshotService::CaptureWorldSnapshot(
    uint64_t aEpochId,
    const std::string& acSlotName,
    bool aIsAuto,
    uint64_t aNowMs) const
{
    WorldSnapshot snap{};
    snap.EpochId = aEpochId;
    snap.SlotName = acSlotName;
    snap.IsAutoCheckpoint = aIsAuto;
    snap.CreatedAtMs = aNowMs;

    // 1. Capture per-player quest logs + authoritative spatial/inventory state (by BOTH stable identity key AND session PlayerId)
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        const std::string identKey = MakePlayerIdentityKey(pPlayer);
        const auto& qlog = pPlayer->GetQuestLogComponent().QuestContent;
        snap.QuestLogsByPlayerId[pPlayer->GetId()] = qlog;
        if (!identKey.empty())
            snap.QuestLogsByIdentityKey[identKey] = qlog;

        if (auto pState = CapturePlayerState(pPlayer); pState.has_value())
        {
            if (!identKey.empty())
                snap.PlayersByIdentityKey[identKey] = *pState;
            snap.PlayersById[pPlayer->GetId()] = std::move(*pState);
        }
        else if (const auto baseIt = m_playerJoinBaselines.find(pPlayer->GetId()); baseIt != m_playerJoinBaselines.end())
        {
            if (!identKey.empty())
                snap.PlayersByIdentityKey[identKey] = baseIt->second;
            snap.PlayersById[pPlayer->GetId()] = baseIt->second;
        }
        else if (!identKey.empty())
        {
            if (const auto identBaseIt = m_playerJoinBaselinesByIdentityKey.find(identKey);
                identBaseIt != m_playerJoinBaselinesByIdentityKey.end())
            {
                snap.PlayersByIdentityKey[identKey] = identBaseIt->second;
                snap.PlayersById[pPlayer->GetId()] = identBaseIt->second;
            }
        }

        for (const auto& entry : qlog.Entries)
        {
            auto it = std::find_if(
                snap.SharedPartyQuestLog.Entries.begin(),
                snap.SharedPartyQuestLog.Entries.end(),
                [&entry](const auto& existing) { return existing.Id == entry.Id; });

            if (it == snap.SharedPartyQuestLog.Entries.end())
                snap.SharedPartyQuestLog.Entries.push_back(entry);
            else if (entry.Stage > it->Stage)
                it->Stage = entry.Stage;
        }
    }

    // 2. Capture all tracked NPC/boss health & alive/dead flags
    auto actorView = m_world.view<CharacterComponent>();
    for (auto entity : actorView)
    {
        const auto& charComp = actorView.get<CharacterComponent>(entity);
        if (charComp.IsPlayer())
            continue;

        ActorStateRecord rec{};
        rec.BaseId = charComp.BaseId.Id;
        if (const auto* pForm = m_world.try_get<FormIdComponent>(entity);
            pForm && static_cast<bool>(pForm->Id) && pForm->Id.ModId != 0xFFFFFFFFu)
        {
            rec.FormId = pForm->Id;
        }
        rec.IsDead = charComp.IsDead();
        if (const auto* pAv = m_world.try_get<ActorValuesComponent>(entity))
        {
            if (const auto hpIt = pAv->CurrentActorValues.ActorValuesList.find(kHealthAvIndex);
                hpIt != pAv->CurrentActorValues.ActorValuesList.end())
            {
                rec.CurrentHealth = hpIt->second;
            }
            if (const auto maxIt = pAv->CurrentActorValues.ActorMaxValuesList.find(kHealthAvIndex);
                maxIt != pAv->CurrentActorValues.ActorMaxValuesList.end())
            {
                rec.MaxHealth = maxIt->second;
            }
        }
        const auto* pMov = m_world.try_get<MovementComponent>(entity);
        const auto* pCell = m_world.try_get<CellIdComponent>(entity);
        if (pMov && pCell && pCell->IsValid())
        {
            rec.HasSpatialState = true;
            rec.CellId = pCell->Cell;
            rec.WorldSpaceId = pCell->WorldSpaceId;
            rec.CenterCoords = pCell->CenterCoords;
            rec.Position = pMov->Position;
            rec.Rotation = pMov->Rotation;
        }
        snap.TrackedActors[World::ToInteger(entity)] = rec;
        if (rec.FormId && rec.FormId.ModId != 0xFFFFFFFFu)
        {
            snap.PersistentActorsByKey[CoopFlight::PackObjectKey(rec.FormId.ModId, rec.FormId.BaseId)] = rec;
        }
    }

    // 3. Capture authoritative RFAB XP ledger, read XP books, globals & ambush history
    snap.Progression = m_world.GetRfabProgressionService().GetLedger().CaptureSnapshot();

    // 4. Capture world calendar state
    const auto& calendar = m_world.GetCalendarService();
    const auto [hour, minute] = calendar.GetTime();
    const auto [day, month, year] = calendar.GetDate();
    snap.Calendar.Hour = hour;
    snap.Calendar.Minute = minute;
    snap.Calendar.TimeScale = calendar.GetTimeScale();
    snap.Calendar.Day = day;
    snap.Calendar.Month = static_cast<int>(month);
    snap.Calendar.Year = static_cast<float>(year);

    // 5. Capture all world containers & lock states (ObjectComponent + InventoryComponent)
    auto objectView = m_world.view<FormIdComponent, ObjectComponent, InventoryComponent>();
    for (auto entity : objectView)
    {
        ObjectStateRecord objRec{};
        objRec.Id = objectView.get<FormIdComponent>(entity).Id;
        objRec.CurrentLockData = objectView.get<ObjectComponent>(entity).CurrentLockData;
        objRec.CurrentInventory = objectView.get<InventoryComponent>(entity).Content;
        if (const auto* pCellComp = m_world.try_get<CellIdComponent>(entity))
        {
            objRec.CellId = pCellComp->Cell;
            objRec.WorldSpaceId = pCellComp->WorldSpaceId;
            objRec.CenterCoords = pCellComp->CenterCoords;
        }
        snap.TrackedObjects.push_back(std::move(objRec));
    }

    // 6. Capture persistent ContainerLootLedger anti-dupe withdrawal history
    snap.LootLedger = m_world.GetInventoryService().CaptureLootLedgerSnapshot();

    return snap;
}

void SaveSnapshotService::FinalizeCommit() noexcept
{
    const uint64_t nowMs = NowSteadyMs();
    const auto committed = m_coordinator.ConsumeAndCommit(nowMs);
    m_deferredManualSaveAfterCombat = false;

    m_snapshots.push_back(CaptureWorldSnapshot(committed.EpochId, committed.SlotName, committed.IsAutoCheckpoint, nowMs));
    while (m_snapshots.size() > SaveSnapshotCoordinator::kMaxSnapshotHistory)
        m_snapshots.pop_front();

    (void)SaveCheckpointsToDisk(GetDefaultCheckpointStorePath());

    NotifyCommitSave commitMsg{};
    commitMsg.EpochId = committed.EpochId;
    commitMsg.SlotName = String(committed.SlotName.c_str());
    commitMsg.Committed = true;
    commitMsg.Message = committed.IsAutoCheckpoint
        ? String("RFAB Online: Чекпоинт после боя сохранён")
        : String("RFAB Online: Сохранение группы завершено");

    if (auto* pServer = GameServer::Get())
        pServer->SendToPlayers(commitMsg, nullptr);
    spdlog::info(
        "[RFAB SaveSnapshot] Committed Epoch={} Slot='{}' (Total snapshots={})",
        committed.EpochId,
        committed.SlotName,
        m_snapshots.size());
}

void SaveSnapshotService::AbortBarrier(const std::string& acReason) noexcept
{
    if (!m_coordinator.HasPendingBarrier())
        return;

    const auto aborted = m_coordinator.ConsumeAborted();

    const std::string lowerReason = RfabCompat::Detail::ToLowerAscii(acReason);
    if (!aborted.IsAutoCheckpoint && (acReason.find("бою") != std::string::npos || lowerReason.find("combat") != std::string::npos))
        m_deferredManualSaveAfterCombat = true;

    NotifyCommitSave abortMsg{};
    abortMsg.EpochId = aborted.EpochId;
    abortMsg.SlotName = String(aborted.SlotName.c_str());
    abortMsg.Committed = false;
    abortMsg.Message = m_deferredManualSaveAfterCombat
        ? String("RFAB Online: Идёт бой — чекпоинт группы сохранится сразу после победы")
        : String(("RFAB Online: Сохранение отложено (" + acReason + ")").c_str());

    if (auto* pServer = GameServer::Get())
        pServer->SendToPlayers(abortMsg, nullptr);
    spdlog::warn("[RFAB SaveSnapshot] Aborted save barrier Epoch={}: {}", aborted.EpochId, acReason);
}

void SaveSnapshotService::ApplyWorldSnapshot(const WorldSnapshot& acSnapshot, bool aIsPartyWipe) noexcept
{
    m_coordinator.NotifyRolledBackTo(acSnapshot.EpochId, acSnapshot.SlotName, NowSteadyMs());

    // 1. Restore RFAB progression ledger
    m_world.GetRfabProgressionService().GetLedger().RestoreSnapshot(acSnapshot.Progression);

    // 2. Restore quest logs for connected players AND flush transient 30s QuestStageDedupHistory + SceneMaster
    auto& partyService = m_world.GetPartyService();
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (PartyService::Party* pParty = partyService.GetPlayerParty(pPlayer))
        {
            pParty->GetQuestStageDedupHistory().Reset();
            pParty->GetQuestStageDedupHistory().ResetSceneMaster();
        }

        const std::string identKey = MakePlayerIdentityKey(pPlayer);
        if (!identKey.empty())
        {
            if (const auto identIt = acSnapshot.QuestLogsByIdentityKey.find(identKey);
                identIt != acSnapshot.QuestLogsByIdentityKey.end())
            {
                pPlayer->GetQuestLogComponent().QuestContent = identIt->second;
                continue;
            }
        }

        if (const auto it = acSnapshot.QuestLogsByPlayerId.find(pPlayer->GetId()); it != acSnapshot.QuestLogsByPlayerId.end())
            pPlayer->GetQuestLogComponent().QuestContent = it->second;
        else
            pPlayer->GetQuestLogComponent().QuestContent = acSnapshot.SharedPartyQuestLog;
    }

    // 3. Restore tracked NPC/boss alive/dead state, spatial coordinates & health on the server + broadcast live resync!
    for (const auto& [serverEntityId, savedState] : acSnapshot.TrackedActors)
    {
        const entt::entity entity = static_cast<entt::entity>(serverEntityId);
        if (!m_world.valid(entity))
            continue;

        bool deathChanged = false;
        if (auto* pChar = m_world.try_get<CharacterComponent>(entity))
        {
            deathChanged = (pChar->IsDead() != savedState.IsDead);
            pChar->SetDead(savedState.IsDead);
        }

        bool spatialChanged = false;
        if (savedState.HasSpatialState)
        {
            if (auto* pMov = m_world.try_get<MovementComponent>(entity))
            {
                const float dx = pMov->Position.x - savedState.Position.x;
                const float dy = pMov->Position.y - savedState.Position.y;
                const float dz = pMov->Position.z - savedState.Position.z;
                if ((dx * dx + dy * dy + dz * dz) > (64.0f * 64.0f))
                    spatialChanged = true;

                pMov->Position = savedState.Position;
                pMov->Rotation = savedState.Rotation;
                pMov->Sent = false;
            }
            if (auto* pCell = m_world.try_get<CellIdComponent>(entity))
            {
                if (pCell->Cell != savedState.CellId || pCell->WorldSpaceId != savedState.WorldSpaceId)
                    spatialChanged = true;

                pCell->Cell = savedState.CellId;
                pCell->WorldSpaceId = savedState.WorldSpaceId;
                pCell->CenterCoords = savedState.CenterCoords;
            }
        }

        bool hpChanged = false;
        float prevHp = savedState.CurrentHealth;
        if (auto* pAv = m_world.try_get<ActorValuesComponent>(entity))
        {
            if (savedState.MaxHealth > 0.0f)
                pAv->CurrentActorValues.ActorMaxValuesList[kHealthAvIndex] = savedState.MaxHealth;
            const auto prevIt = pAv->CurrentActorValues.ActorValuesList.find(kHealthAvIndex);
            prevHp = prevIt != pAv->CurrentActorValues.ActorValuesList.end() ? prevIt->second : savedState.CurrentHealth;
            pAv->CurrentActorValues.ActorValuesList[kHealthAvIndex] = savedState.CurrentHealth;
            hpChanged = std::abs(prevHp - savedState.CurrentHealth) > 0.05f;
        }

        if (deathChanged)
        {
            NotifyDeathStateChange deathNotify{};
            deathNotify.Id = serverEntityId;
            deathNotify.IsDead = savedState.IsDead;
            if (auto* pServer = GameServer::Get())
                pServer->SendToPlayers(deathNotify, nullptr);
        }

        if (spatialChanged && !savedState.IsDead)
        {
            if (const auto* pFormComp = m_world.try_get<FormIdComponent>(entity);
                pFormComp && static_cast<bool>(pFormComp->Id) && pFormComp->Id.ModId != 0xFFFFFFFFu)
            {
                NotifyActorTeleport teleportNotify{};
                teleportNotify.FormId = pFormComp->Id;
                teleportNotify.WorldSpaceId = savedState.WorldSpaceId;
                teleportNotify.CellId = savedState.CellId;
                teleportNotify.Position = savedState.Position;
                if (auto* pServer = GameServer::Get())
                    pServer->SendToPlayers(teleportNotify, nullptr);
            }
        }

        if (hpChanged && !savedState.IsDead)
        {
            NotifyHealthChangeBroadcast hpNotify{};
            hpNotify.Id = serverEntityId;
            hpNotify.DeltaHealth = savedState.CurrentHealth - prevHp;
            if (auto* pServer = GameServer::Get())
                pServer->SendToPlayers(hpNotify, nullptr);
        }
    }

    // 3a-persistent. Ensure persistent placed actors keyed by FormId are restored even if their
    // transient entt::entity handle changed or was recreated across a server restart / cell unload.
    for (const auto& [_, savedState] : acSnapshot.PersistentActorsByKey)
    {
        if (!savedState.FormId || savedState.FormId.ModId == 0xFFFFFFFFu || !savedState.HasSpatialState)
            continue;

        (void)m_world.GetCharacterService().EnsureDormantPersistentCharacter(
            savedState.FormId,
            savedState.BaseId,
            savedState.CellId,
            savedState.WorldSpaceId,
            savedState.CenterCoords,
            savedState.Position,
            savedState.Rotation,
            savedState.IsDead,
            savedState.CurrentHealth,
            savedState.MaxHealth);
    }

    // 3b. Restore world containers & lock states (ObjectComponent + InventoryComponent)
    {
        auto objectView = m_world.view<FormIdComponent, ObjectComponent, InventoryComponent>();
        std::vector<entt::entity> postCheckpointObjects;
        for (auto entity : objectView)
        {
            const GameId& objId = objectView.get<FormIdComponent>(entity).Id;
            auto it = std::find_if(
                acSnapshot.TrackedObjects.begin(),
                acSnapshot.TrackedObjects.end(),
                [&objId](const ObjectStateRecord& rec) { return rec.Id == objId; });

            if (it == acSnapshot.TrackedObjects.end())
            {
                postCheckpointObjects.push_back(entity);
            }
        }
        for (entt::entity entity : postCheckpointObjects)
        {
            if (m_world.valid(entity))
            {
                m_world.GetInventoryService().ForgetContainerForEntity(entity, World::ToInteger(entity));
                m_world.destroy(entity);
            }
        }
        for (const auto& objRec : acSnapshot.TrackedObjects)
        {
            m_world.GetObjectService().UpsertRestoredObject(
                objRec.Id,
                objRec.CellId,
                objRec.WorldSpaceId,
                objRec.CenterCoords,
                objRec.CurrentLockData,
                objRec.CurrentInventory);
        }
    }

    // 4. Restore server calendar date & time
    auto& calendar = m_world.GetCalendarService();
    calendar.SetDate(acSnapshot.Calendar.Day, acSnapshot.Calendar.Month, acSnapshot.Calendar.Year);
    calendar.SetTime(acSnapshot.Calendar.Hour, acSnapshot.Calendar.Minute, acSnapshot.Calendar.TimeScale);

    // 5. Clear downed/revive/wipe states and restore container anti-dupe ledger to exact checkpoint epoch
    m_world.GetEncounterService().ResetAllStatesAfterRollback();
    m_world.GetInventoryService().RestoreLootLedgerSnapshot(acSnapshot.LootLedger);

    // 6. Drop snapshots strictly newer than the restored epoch & persist updated store to disk
    while (!m_snapshots.empty() && m_snapshots.back().EpochId > acSnapshot.EpochId)
        m_snapshots.pop_back();
    (void)SaveCheckpointsToDisk(GetDefaultCheckpointStorePath());

    // 7a. Restore EVERY player's server state & dispatch NotifyExecuteLoad FIRST so clients teleport to
    // checkpoint coordinates and flush pre-rollback interpolation BEFORE receiving fresh observer spawns!
    std::vector<Player*> inSessionPlayers;
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (!pPlayer)
            continue;

        const PlayerStateRecord* pPlayerState = nullptr;
        const std::string identKey = MakePlayerIdentityKey(pPlayer);
        if (!identKey.empty())
        {
            if (const auto identIt = acSnapshot.PlayersByIdentityKey.find(identKey);
                identIt != acSnapshot.PlayersByIdentityKey.end())
            {
                pPlayerState = &identIt->second;
            }
        }
        if (!pPlayerState)
        {
            if (const auto it = acSnapshot.PlayersById.find(pPlayer->GetId()); it != acSnapshot.PlayersById.end())
                pPlayerState = &it->second;
        }
        if (!pPlayerState && !identKey.empty())
        {
            if (const auto identBaseIt = m_playerJoinBaselinesByIdentityKey.find(identKey);
                identBaseIt != m_playerJoinBaselinesByIdentityKey.end())
            {
                pPlayerState = &identBaseIt->second;
            }
        }
        if (!pPlayerState)
        {
            if (const auto baseIt = m_playerJoinBaselines.find(pPlayer->GetId()); baseIt != m_playerJoinBaselines.end())
                pPlayerState = &baseIt->second;
        }

        const bool hasTransform = pPlayerState && pPlayerState->HasValidTransform;
        const bool useInSession = CoopCheckpoint::ShouldServerPreferInSessionRollback(aIsPartyWipe, hasTransform, acSnapshot.SlotName);

        if (useInSession && pPlayerState && pPlayer->GetCharacter().has_value())
        {
            const entt::entity charEntity = *pPlayer->GetCharacter();
            if (m_world.valid(charEntity))
            {
                if (pPlayerState->HasValidTransform)
                {
                    if (auto* pMov = m_world.try_get<MovementComponent>(charEntity))
                    {
                        pMov->Position = pPlayerState->Position;
                        pMov->Rotation = pPlayerState->Rotation;
                        pMov->Sent = false;
                    }
                    CellIdComponent restoredCell{pPlayerState->CellId, pPlayerState->WorldSpaceId, pPlayerState->CenterCoords};
                    pPlayer->SetCellComponent(restoredCell);
                    m_world.emplace_or_replace<CellIdComponent>(charEntity, restoredCell);
                }
                if (pPlayerState->HasInventory)
                {
                    if (auto* pInv = m_world.try_get<InventoryComponent>(charEntity))
                        pInv->Content = pPlayerState->CurrentInventory;
                }
                inSessionPlayers.push_back(pPlayer);
            }
        }

        NotifyExecuteLoad loadCmd{};
        loadCmd.EpochId = acSnapshot.EpochId;
        loadCmd.SlotName = String(acSnapshot.SlotName.c_str());
        loadCmd.IsPartyWipe = aIsPartyWipe;
        loadCmd.AllowPreJoinFallback = !hasTransform;
        loadCmd.UseInSessionRollback = useInSession;
        loadCmd.RestoreHealthFraction = 1.0f;
        if (pPlayerState && pPlayerState->HasValidTransform)
        {
            loadCmd.HasRestoreTransform = true;
            loadCmd.TargetCellId = pPlayerState->CellId;
            loadCmd.TargetWorldSpaceId = pPlayerState->WorldSpaceId;
            loadCmd.PositionX = pPlayerState->Position.x;
            loadCmd.PositionY = pPlayerState->Position.y;
            loadCmd.PositionZ = pPlayerState->Position.z;
            loadCmd.RotationZ = pPlayerState->Rotation.z;
        }
        if (pPlayerState && pPlayerState->HasInventory)
        {
            loadCmd.HasRestoreInventory = true;
            loadCmd.RestoreInventory = pPlayerState->CurrentInventory;
        }
        loadCmd.Message = useInSession
            ? String("RFAB Online: Отряд пал — мгновенный возврат к чекпоинту сессии")
            : (aIsPartyWipe
                ? String("RFAB Online: Отряд пал — возврат к последнему чекпоинту")
                : String("RFAB Online: Синхронная загрузка чекпоинта группы"));

        pPlayer->Send(loadCmd);
    }

    // 7b. Now that ALL players' checkpoint CellIdComponents & MovementComponents are updated and NotifyExecuteLoad
    // is ahead in each peer's reliable stream, force full observer interest resync so surviving NPCs & peers
    // re-replicate with authoritative post-rollback coordinates!
    for (Player* pPlayer : inSessionPlayers)
    {
        m_world.GetCharacterService().ForceResyncObserverAfterRollback(pPlayer);
    }

    // Broadcast monotonic WorldGeneration marker & authoritative RFAB globals
    const uint64_t worldGen = m_coordinator.GetCurrentWorldEpoch();
    auto& ledger = m_world.GetRfabProgressionService().GetLedger();
    if (auto* pServer = GameServer::Get())
    {
        for (const auto& [editorId, val] : ledger.GetSyncedGlobals())
        {
            NotifyRfabProgressionEvent globalResync{};
            globalResync.Kind = NotifyRfabProgressionEvent::EventKind::GlobalValueSync;
            globalResync.EpochId = worldGen;
            globalResync.EditorId = String(editorId.c_str());
            globalResync.GlobalValue = val;
            globalResync.AuthoritativeTotalXp = ledger.GetTotalSharedXp();
            pServer->SendToPlayers(globalResync, nullptr);
        }

        NotifyRfabProgressionEvent epochMarker{};
        epochMarker.Kind = NotifyRfabProgressionEvent::EventKind::LedgerXpGrant;
        epochMarker.EpochId = worldGen;
        epochMarker.XpDelta = 0;
        epochMarker.AuthoritativeTotalXp = ledger.GetTotalSharedXp();
        pServer->SendToPlayers(epochMarker, nullptr);
    }

    spdlog::info(
        "[RFAB SaveSnapshot] Rolled back server world Epoch={} WorldGen={} Slot='{}' (PartyWipe={}, Digest={})",
        acSnapshot.EpochId,
        worldGen,
        acSnapshot.SlotName,
        aIsPartyWipe,
        BuildLiveWorldDigest().ComputeHexDigest());
}

bool SaveSnapshotService::TriggerRollbackToLatest(bool aIsPartyWipe) noexcept
{
    RefreshPlayerJoinBaselines();

    if (!m_snapshots.empty())
    {
        const WorldSnapshot target = m_snapshots.back();
        ApplyWorldSnapshot(target, aIsPartyWipe);
        return true;
    }

    // Pre-checkpoint rollback: seamlessly restore every player to their current-session join baseline WITHOUT
    // disconnecting or loading any stale .ess save from an older session!
    m_coordinator.NotifyRolledBackTo(0, "", NowSteadyMs());
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (PartyService::Party* pParty = m_world.GetPartyService().GetPlayerParty(pPlayer))
        {
            pParty->GetQuestStageDedupHistory().Reset();
            pParty->GetQuestStageDedupHistory().ResetSceneMaster();
        }
    }
    m_world.GetEncounterService().ResetAllStatesAfterRollback();
    m_world.GetInventoryService().ResetLootLedger();
    {
        auto objectView = m_world.view<ObjectComponent>();
        std::vector<entt::entity> toDestroy(objectView.begin(), objectView.end());
        for (entt::entity entity : toDestroy)
        {
            if (m_world.valid(entity))
                m_world.destroy(entity);
        }
    }

    std::vector<Player*> baselinePlayers;
    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        if (!pPlayer)
            continue;

        const auto baseIt = m_playerJoinBaselines.find(pPlayer->GetId());
        const PlayerStateRecord* pBaseline = (baseIt != m_playerJoinBaselines.end()) ? &baseIt->second : nullptr;
        const bool hasBaselineTransform = pBaseline && pBaseline->HasValidTransform;

        if (hasBaselineTransform && pPlayer->GetCharacter().has_value())
        {
            const entt::entity charEntity = *pPlayer->GetCharacter();
            if (m_world.valid(charEntity))
            {
                if (auto* pMov = m_world.try_get<MovementComponent>(charEntity))
                {
                    pMov->Position = pBaseline->Position;
                    pMov->Rotation = pBaseline->Rotation;
                    pMov->Sent = false;
                }
                CellIdComponent restoredCell{pBaseline->CellId, pBaseline->WorldSpaceId, pBaseline->CenterCoords};
                pPlayer->SetCellComponent(restoredCell);
                m_world.emplace_or_replace<CellIdComponent>(charEntity, restoredCell);
                if (pBaseline->HasInventory)
                {
                    if (auto* pInv = m_world.try_get<InventoryComponent>(charEntity))
                        pInv->Content = pBaseline->CurrentInventory;
                }
                baselinePlayers.push_back(pPlayer);
            }
        }

        NotifyExecuteLoad fallbackLoad{};
        fallbackLoad.EpochId = 0;
        fallbackLoad.SlotName = "";
        fallbackLoad.IsPartyWipe = aIsPartyWipe;
        fallbackLoad.AllowPreJoinFallback = !hasBaselineTransform;
        fallbackLoad.UseInSessionRollback = true;
        fallbackLoad.RestoreHealthFraction = 1.0f;
        if (hasBaselineTransform)
        {
            fallbackLoad.HasRestoreTransform = true;
            fallbackLoad.TargetCellId = pBaseline->CellId;
            fallbackLoad.TargetWorldSpaceId = pBaseline->WorldSpaceId;
            fallbackLoad.PositionX = pBaseline->Position.x;
            fallbackLoad.PositionY = pBaseline->Position.y;
            fallbackLoad.PositionZ = pBaseline->Position.z;
            fallbackLoad.RotationZ = pBaseline->Rotation.z;
        }
        if (pBaseline && pBaseline->HasInventory)
        {
            fallbackLoad.HasRestoreInventory = true;
            fallbackLoad.RestoreInventory = pBaseline->CurrentInventory;
        }
        fallbackLoad.Message = String("RFAB Online: Отряд пал — возврат к стартовой точке сессии");
        pPlayer->Send(fallbackLoad);
    }

    for (Player* pPlayer : baselinePlayers)
    {
        m_world.GetCharacterService().ForceResyncObserverAfterRollback(pPlayer);
    }

    if (auto* pServer = GameServer::Get())
    {
        NotifyRfabProgressionEvent epochMarker{};
        epochMarker.Kind = NotifyRfabProgressionEvent::EventKind::LedgerXpGrant;
        epochMarker.EpochId = m_coordinator.GetCurrentWorldEpoch();
        epochMarker.XpDelta = 0;
        epochMarker.AuthoritativeTotalXp = m_world.GetRfabProgressionService().GetLedger().GetTotalSharedXp();
        pServer->SendToPlayers(epochMarker, nullptr);
    }

    spdlog::info("[RFAB SaveSnapshot] Triggered seamless in-session baseline rollback (PartyWipe={})", aIsPartyWipe);
    return true;
}

bool SaveSnapshotService::TriggerRollbackToPrevious() noexcept
{
    if (m_snapshots.size() < 2)
        return TriggerRollbackToLatest(false);

    const WorldSnapshot target = m_snapshots[m_snapshots.size() - 2];
    ApplyWorldSnapshot(target, false);
    return true;
}

bool SaveSnapshotService::TriggerRollbackToSlot(const std::string& acSlotName, bool aIsPartyWipe) noexcept
{
    const std::string normTarget = RfabCompat::Detail::NormalizeSaveSlotStem(acSlotName);
    for (auto it = m_snapshots.rbegin(); it != m_snapshots.rend(); ++it)
    {
        if (!normTarget.empty() && RfabCompat::Detail::NormalizeSaveSlotStem(it->SlotName) == normTarget)
        {
            const WorldSnapshot target = *it;
            ApplyWorldSnapshot(target, aIsPartyWipe);
            return true;
        }
    }

    return TriggerRollbackToLatest(aIsPartyWipe);
}

void SaveSnapshotService::NotifyEncounterVictory(bool aBossDefeated) noexcept
{
    if (m_deferredManualSaveAfterCombat)
    {
        m_deferredManualSaveAfterCombat = false;
        TriggerCoordinatedSave(false, nullptr);
        return;
    }

    const uint64_t nowMs = NowSteadyMs();
    const uint32_t intervalSec = uPostCombatAutosaveInterval.value_as<uint32_t>();
    if (!m_coordinator.ShouldTriggerPostCombatAutosave(nowMs, intervalSec, aBossDefeated))
        return;

    TriggerCoordinatedSave(true, nullptr);
}

void SaveSnapshotService::OnUpdate(const UpdateEvent&) noexcept
{
    RefreshPlayerJoinBaselines();

    if (m_coordinator.HasBarrierTimedOut(NowSteadyMs()))
        AbortBarrier("таймаут ожидания одного из игроков в меню/загрузке");
}

void SaveSnapshotService::OnPlayerLeave(const PlayerLeaveEvent& acEvent) noexcept
{
    if (!acEvent.pPlayer)
        return;

    m_playerJoinBaselines.erase(acEvent.pPlayer->GetId());

    const auto res = m_coordinator.HandlePlayerDisconnected(acEvent.pPlayer->GetId());
    if (res == SaveSnapshotCoordinator::AckResolution::ReadyToCommit)
        FinalizeCommit();
    else if (res == SaveSnapshotCoordinator::AckResolution::Aborted)
        AbortBarrier("игрок отключился");
}

void SaveSnapshotService::OnSaveLoadRequest(const PacketEvent<RequestCoordinatedSaveLoad>& acMessage) noexcept
{
    Player* pSender = acMessage.pPlayer;
    if (!pSender)
        return;

    const auto& req = acMessage.Packet;

    if (req.Action == RequestCoordinatedSaveLoad::ActionType::ManualSave)
    {
        TriggerCoordinatedSave(false, nullptr);
        return;
    }

    if (!IsPlayerAuthorizedHost(pSender))
    {
        spdlog::debug("[RFAB SaveSnapshot] Ignoring non-host load request from player {}", pSender->GetId());
        return;
    }

    switch (req.Action)
    {
    case RequestCoordinatedSaveLoad::ActionType::ManualSave:
        TriggerCoordinatedSave(false, nullptr);
        break;
    case RequestCoordinatedSaveLoad::ActionType::LoadLatestCheckpoint:
        TriggerRollbackToLatest(false);
        break;
    case RequestCoordinatedSaveLoad::ActionType::LoadPreviousCheckpoint:
        TriggerRollbackToPrevious();
        break;
    case RequestCoordinatedSaveLoad::ActionType::LoadSpecificSlot:
        TriggerRollbackToSlot(std::string(req.SlotName.c_str(), req.SlotName.size()), false);
        break;
    }
}

void SaveSnapshotService::OnSaveAck(const PacketEvent<CoordinatedSaveAck>& acMessage) noexcept
{
    Player* pSender = acMessage.pPlayer;
    if (!pSender)
        return;

    const auto& ack = acMessage.Packet;
    const std::string reason(ack.Reason.c_str(), ack.Reason.size());
    const auto res = m_coordinator.RecordClientAck(pSender->GetId(), ack.EpochId, ack.Success, reason);
    if (res == SaveSnapshotCoordinator::AckResolution::ReadyToCommit)
        FinalizeCommit();
    else if (res == SaveSnapshotCoordinator::AckResolution::Aborted)
        AbortBarrier(reason);
}

CoopFlight::BucketedStateDigest SaveSnapshotService::BuildLiveWorldDigest() const
{
    CoopFlight::BucketedStateDigest digest;

    const auto& ledger = m_world.GetRfabProgressionService().GetLedger();
    digest.Put(CoopFlight::StateCategory::kSharedXp, "total_xp", static_cast<uint64_t>(ledger.GetTotalSharedXp()));

    for (const auto& bookId : ledger.GetReadBooks())
    {
        char key[32]{};
        std::snprintf(key, sizeof(key), "%08X:%08X", bookId.ModId, bookId.BaseId);
        digest.Put(CoopFlight::StateCategory::kReadBook, key, 1ULL);
    }

    for (const auto& [editorId, val] : ledger.GetSyncedGlobals())
    {
        const auto quantized = static_cast<uint64_t>(static_cast<int64_t>(val * 1000.0f));
        digest.Put(CoopFlight::StateCategory::kSyncedGlobal, editorId, quantized);
    }

    for (Player* pPlayer : m_world.GetPlayerManager())
    {
        for (const auto& q : pPlayer->GetQuestLogComponent().QuestContent.Entries)
        {
            char key[32]{};
            std::snprintf(key, sizeof(key), "%08X:%08X", q.Id.ModId, q.Id.BaseId);
            digest.Put(CoopFlight::StateCategory::kQuestStage, key, static_cast<uint64_t>(q.Stage));
        }
    }

    auto ownerView = m_world.view<OwnerComponent>();
    for (auto entity : ownerView)
    {
        const auto& ownerComp = ownerView.get<OwnerComponent>(entity);
        char key[24]{};
        std::snprintf(key, sizeof(key), "%08X", World::ToInteger(entity));
        const uint32_t ownerId = ownerComp.GetOwner() ? ownerComp.GetOwner()->GetId() : 0;
        const uint64_t combined = (ownerComp.Epoch << 16) ^ static_cast<uint64_t>(ownerId);
        digest.Put(CoopFlight::StateCategory::kActorOwnership, key, combined);
    }

    digest.Put(
        CoopFlight::StateCategory::kSaveCheckpoint,
        m_coordinator.GetLastCommittedSlot().empty() ? "none" : m_coordinator.GetLastCommittedSlot(),
        m_coordinator.GetLastCommittedEpoch());

    return digest;
}
