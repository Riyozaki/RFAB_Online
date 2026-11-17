// Phase 0 in-process server harness: compiles and executes the real shipping Code/server
// (upstream TiltedEvolution + Phase 0 stability fixes) on Linux under ASan + UBSan.
//
// Nothing mocks the server's services or World: only the socket transport (TiltedPhoques::Server)
// is replaced by an outbox recorder (ConnectStub.cpp), and every scenario drives the real
// wire serializer/deserializer + entt::dispatcher.

#include <spdlog/sinks/stdout_color_sinks.h>

#include <ConsoleRegistry.h>
#include <Components.h>
#include <GameServer.h>
#include <World.h>

#include <Events/PacketEvent.h>
#include <Events/PlayerJoinEvent.h>
#include <Events/PlayerLeaveEvent.h>
#include <Events/UpdateEvent.h>

#include <Messages/ActivateRequest.h>
#include <Messages/AssignCharacterRequest.h>
#include <Messages/AssignObjectsRequest.h>
#include <Messages/AssignObjectsResponse.h>
#include <Messages/RequestInventorySnapshot.h>
#include <Messages/AssignCharacterResponse.h>
#include <Messages/CharacterSpawnRequest.h>
#include <Messages/ClientMessageFactory.h>
#include <Messages/ClientReferencesMoveRequest.h>
#include <Messages/DrawWeaponRequest.h>
#include <Messages/EnterInteriorCellRequest.h>
#include <Messages/LockChangeRequest.h>
#include <Messages/NotifyActivate.h>
#include <Messages/NotifyActorMaxValueChanges.h>
#include <Messages/NotifyActorValueChanges.h>
#include <Messages/NotifyChatMessageBroadcast.h>
#include <Messages/NotifyDeathStateChange.h>
#include <Messages/NotifyDrawWeapon.h>
#include <Messages/NotifyHealthChangeBroadcast.h>
#include <Messages/NotifyLockChange.h>
#include <Messages/NotifyOwnershipTransfer.h>
#include <Messages/NotifyPartyInfo.h>
#include <Messages/NotifyPartyJoined.h>
#include <Messages/NotifyQuestUpdate.h>
#include <Messages/PartyAcceptInviteRequest.h>
#include <Messages/PartyCreateRequest.h>
#include <Messages/PartyInviteRequest.h>
#include <Messages/RequestActionEvents.h>
#include <Messages/RequestInventoryChanges.h>
#include <Messages/NotifyInventoryChanges.h>
#include <Messages/RequestActorMaxValueChanges.h>
#include <Messages/RequestActorValueChanges.h>
#include <Messages/RequestDeathStateChange.h>
#include <Messages/RequestHealthChangeBroadcast.h>
#include <Messages/RequestOwnershipClaim.h>
#include <Messages/RequestOwnershipTransfer.h>
#include <Messages/RequestQuestUpdate.h>
#include <Messages/SendChatMessageRequest.h>
#include <Messages/ServerMessageFactory.h>
#include <Messages/ServerReferencesMoveRequest.h>

#include <Services/CharacterService.h>
#include <Services/CoopService.h>
#include <Messages/RequestCoopAction.h>
#include <Messages/NotifyCoopAction.h>
#include <Services/PartyService.h>

#include "TransportSpy.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;

void Check(bool aCondition, const char* acWhat)
{
    if (aCondition)
    {
        std::printf("  ok   %s\n", acWhat);
        return;
    }
    std::printf("  FAIL %s\n", acWhat);
    ++g_failures;
}

class TemporaryWorkingDirectory
{
public:
    TemporaryWorkingDirectory()
    {
        std::error_code ec;
        m_original = std::filesystem::current_path(ec);
        const auto seed = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i = 0; i < 100; ++i)
        {
            m_path = std::filesystem::temp_directory_path(ec) /
                     ("rfab-phase0-server-" + std::to_string(seed) + "-" + std::to_string(i));
            if (std::filesystem::create_directory(m_path, ec) && !ec)
            {
                std::filesystem::current_path(m_path, ec);
                return;
            }
        }
    }

    ~TemporaryWorkingDirectory()
    {
        std::error_code ec;
        if (!m_original.empty())
            std::filesystem::current_path(m_original, ec);
        if (!m_path.empty())
            std::filesystem::remove_all(m_path, ec);
    }

private:
    std::filesystem::path m_original;
    std::filesystem::path m_path;
};

template <class T> bool DeliverWire(World& aWorld, const T& acOutgoing, Player* apSender)
{
    TiltedPhoques::Buffer raw(65536);
    TiltedPhoques::Buffer::Writer writer(&raw);
    acOutgoing.Serialize(writer);

    TiltedPhoques::Buffer::Reader reader(&raw);
    const ClientMessageFactory factory;
    auto pExtracted = factory.Extract(reader);
    if (!pExtracted || pExtracted->GetOpcode() != acOutgoing.GetOpcode())
        return false;

    auto* pTyped = static_cast<T*>(pExtracted.get());
    aWorld.GetDispatcher().enqueue<PacketEvent<T>>(pTyped, apSender);
    aWorld.GetDispatcher().update();
    return true;
}

template <class TServerMsg>
std::vector<TServerMsg> DecodeOutboxMessagesFor(TiltedPhoques::ConnectionId_t aConn)
{
    std::vector<TServerMsg> decoded;
    const ServerMessageFactory factory;
    for (const auto& pkt : Harness::Outbox())
    {
        if (pkt.Connection != aConn && pkt.Connection != k_HSteamNetConnection_Invalid)
            continue;
        if (pkt.Bytes.empty())
            continue;

        TiltedPhoques::Buffer buf(pkt.Bytes.size() + 16);
        std::memset(buf.GetWriteData(), 0, pkt.Bytes.size() + 16);
        std::memcpy(buf.GetWriteData(), pkt.Bytes.data(), pkt.Bytes.size());
        TiltedPhoques::Buffer::Reader reader(&buf);
        auto pMsg = factory.Extract(reader);
        if (pMsg && pMsg->GetOpcode() == TServerMsg::Opcode)
            decoded.push_back(*static_cast<const TServerMsg*>(pMsg.get()));
    }
    return decoded;
}

struct TransportAccess : TiltedPhoques::Server
{
    using TiltedPhoques::Server::OnDisconnection;
};

void Disconnect(GameServer& aServer, TiltedPhoques::ConnectionId_t aConnectionId)
{
    TiltedPhoques::Server& transport = aServer;
    (transport.*(&TransportAccess::OnDisconnection))(aConnectionId, TiltedPhoques::Server::EDisconnectReason::Quit);
}

Player* JoinPlayer(World& aWorld, TiltedPhoques::ConnectionId_t aConn, const char* apName, GameId aCellId,
                   uint32_t aCookie, const Inventory* apInventory = nullptr)
{
    Player* pPlayer = aWorld.GetPlayerManager().Create(aConn);
    if (!pPlayer)
        return nullptr;

    pPlayer->SetUsername(apName);
    pPlayer->SetLevel(10);
    const auto skyrim = aWorld.ctx().at<ModsComponent>().AddStandard("Skyrim.esm");
    pPlayer->SetMods({"Skyrim.esm"});
    pPlayer->SetModIds({static_cast<uint16_t>(skyrim)});

    CellIdComponent cell{};
    cell.Cell = aCellId;
    pPlayer->SetCellComponent(cell);

    aWorld.GetDispatcher().trigger(PlayerJoinEvent(pPlayer));

    // Assign player character via real wire message so CharacterService creates entity + OwnerComponent.
    // Upstream requires FormId == GameId{} for player characters (ReferenceId == 0:0x14).
    AssignCharacterRequest req{};
    req.Cookie = aCookie;
    req.ReferenceId = GameId(0, 0x14);
    req.FormId = GameId{};
    req.CellId = aCellId;
    req.Position.x = 100.f;
    req.Position.y = 200.f;
    req.Position.z = 0.f;
    if (apInventory) req.CurrentActorData.InitialInventory = *apInventory;
    req.CurrentActorData.InitialActorValues.ActorValuesList[24] = 100.f;
    req.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = 100.f;
    DeliverWire(aWorld, req, pPlayer);

    return pPlayer;
}

void RunEntityZeroAndLifecycleScenario(Console::ConsoleRegistry& aConsole)
{
    std::printf("[scenario 1] entity 0 reservation + World destruction order\n");
    {
        GameServer server(aConsole);
        auto& world = server.GetWorld();

        const entt::entity e1 = world.create();
        Check(e1 != entt::entity{0}, "world.create() never returns reserved entity 0");
        world.destroy(e1);

        // Create and populate an entity right before destroying GameServer/World to exercise
        // WorldParts::DispatcherOwner destruction order under ASan/UBSan.
        auto* pAlice = JoinPlayer(world, 1001, "Alice", GameId(0, 0x165A8), 11);
        Check(pAlice != nullptr && pAlice->GetCharacter().has_value(), "player joins and receives a character entity");
        if (pAlice && pAlice->GetCharacter())
            Check(*pAlice->GetCharacter() != entt::entity{0}, "player character entity is never 0");
    }
    Check(true, "GameServer and World tear down cleanly with live entities and signal connections");
}

void RunTwoPlayerSyncAndOwnershipScenario(Console::ConsoleRegistry& aConsole)
{
    std::printf("[scenario 2] two-player assignment, ownership epochs, health/death, and disconnect handoff\n");
    GameServer server(aConsole);
    auto& world = server.GetWorld();

    const GameId kBanneredMare(0, 0x165A8);
    constexpr TiltedPhoques::ConnectionId_t kAliceConn = 2001;
    constexpr TiltedPhoques::ConnectionId_t kBobConn = 2002;

    Player* pAlice = JoinPlayer(world, kAliceConn, "Alice", kBanneredMare, 101);
    Player* pBob = JoinPlayer(world, kBobConn, "Bob", kBanneredMare, 102);
    Check(pAlice != nullptr && pBob != nullptr, "both players join into the same interior cell");

    // Party flow
    Harness::ClearOutbox();
    PartyCreateRequest createParty{};
    Check(DeliverWire(world, createParty, pAlice), "Alice creates a party");
    PartyInviteRequest invite{};
    invite.PlayerId = pBob->GetId();
    Check(DeliverWire(world, invite, pAlice), "Alice invites Bob");
    PartyAcceptInviteRequest accept{};
    accept.InviterId = pAlice->GetId();
    Check(DeliverWire(world, accept, pBob), "Bob accepts the invite");
    Check(pAlice->GetParty().JoinedPartyId != -1 &&
              pAlice->GetParty().JoinedPartyId == pBob->GetParty().JoinedPartyId,
          "Alice and Bob share the same party ID");

    // Assign a shared NPC (Bandit) owned by Alice
    Harness::ClearOutbox();
    AssignCharacterRequest npcReq{};
    npcReq.Cookie = 5001;
    npcReq.ReferenceId = GameId(0, 0x20001);
    npcReq.FormId = GameId(0, 0x30001);
    npcReq.CellId = kBanneredMare;
    npcReq.Position.x = 120.f;
    npcReq.Position.y = 220.f;
    npcReq.Position.z = 0.f;
    npcReq.CurrentActorData.InitialActorValues.ActorValuesList[24] = 120.f;
    npcReq.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = 150.f;
    Check(DeliverWire(world, npcReq, pAlice), "Alice assigns shared NPC over wire");

    const auto responses = DecodeOutboxMessagesFor<AssignCharacterResponse>(kAliceConn);
    Check(responses.size() == 1 && responses[0].Owner && responses[0].ServerId != 0 && responses[0].OwnershipEpoch != 0,
          "Alice receives AssignCharacterResponse as owner with non-zero ServerId and OwnershipEpoch");

    const uint32_t npcServerId = responses.empty() ? 0u : responses[0].ServerId;
    const uint32_t initialEpoch = responses.empty() ? 0u : responses[0].OwnershipEpoch;
    const auto npcEntity = static_cast<entt::entity>(npcServerId);

    // Valid owner update with current epoch succeeds and relays to Bob
    Harness::ClearOutbox();
    RequestActorValueChanges validAv{};
    validAv.Id = npcServerId;
    validAv.OwnershipEpoch = initialEpoch;
    validAv.Values[25] = 120.f;
    Check(DeliverWire(world, validAv, pAlice), "owner sends valid actor value update");
    const auto bobAvNotifies = DecodeOutboxMessagesFor<NotifyActorValueChanges>(kBobConn);
    Check(bobAvNotifies.size() == 1 && std::fabs(bobAvNotifies[0].Values.at(25) - 120.f) < 0.01f,
          "Bob receives NotifyActorValueChanges with updated non-health AV 120");

    // Regression: signed health operations, not merely malformed floats.
    RequestHealthChangeBroadcast damage{};
    damage.Id = npcServerId;
    damage.OwnershipEpoch = initialEpoch;
    damage.Sequence = 1;
    damage.DeltaHealth = -10.f;
    Harness::ClearOutbox();
    Check(DeliverWire(world, damage, pBob), "finite damage crosses the production wire path");
    Check(std::fabs(world.get<ActorValuesComponent>(npcEntity).CurrentActorValues.ActorValuesList.at(24) - 110.f) < 0.01f,
          "120 HP plus signed -10 damage is 110, never 130");
    validAv.Values[25] = 120.f;
    DeliverWire(world, validAv, pAlice);
    damage.Sequence = 2;
    damage.DeltaHealth = +20.f;
    Check(DeliverWire(world, damage, pBob), "finite healing crosses the production wire path");
    Check(std::fabs(world.get<ActorValuesComponent>(npcEntity).CurrentActorValues.ActorValuesList.at(24) - 130.f) < 0.01f,
          "110 HP plus signed +20 healing is 130");
    Harness::ClearOutbox();
    DeliverWire(world, damage, pBob);
    Check(std::fabs(world.get<ActorValuesComponent>(npcEntity).CurrentActorValues.ActorValuesList.at(24) - 130.f) < 0.01f,
          "duplicate sequenced healing is acknowledged but never applied twice");
    auto staleHealth = damage;
    staleHealth.Sequence = 3;
    staleHealth.OwnershipEpoch = initialEpoch + 1;
    staleHealth.DeltaHealth = -100.f;
    Harness::ClearOutbox();
    DeliverWire(world, staleHealth, pBob);
    Check(DecodeOutboxMessagesFor<NotifyHealthChangeBroadcast>(kAliceConn).empty(), "wrong-epoch finite damage is not relayed");
    validAv.Values[24] = 1.f;
    DeliverWire(world, validAv, pAlice);
    Check(world.get<ActorValuesComponent>(npcEntity).CurrentActorValues.ActorValuesList.at(24) == 130.f,
          "periodic absolute HP cannot overwrite the canonical signed-mutation stream");
    damage.Sequence = 3;
    damage.DeltaHealth = -10.f;
    DeliverWire(world, damage, pBob); // restore the fixture to 120 via its actual production contract

    // Regression: the second take of a single stack must not create a negative inventory or be broadcast.
    Inventory::Entry item{};
    item.BaseId = GameId(0, 0x12eb7);
    item.Count = 1;
    world.get<InventoryComponent>(npcEntity).Content.AddOrRemoveEntry(item);
    RequestInventoryChanges take{};
    take.ServerId = npcServerId;
    take.OwnershipEpoch = initialEpoch;
    take.UpdateClients = true;
    take.Item = item;
    take.Item.Count = -1;
    DeliverWire(world, take, pAlice);
    Harness::ClearOutbox();
    DeliverWire(world, take, pBob);
    Check(world.get<InventoryComponent>(npcEntity).Content.GetEntryCountById(item.BaseId) == 0,
          "two takes of the last item cannot store a negative stack");
    Check(DecodeOutboxMessagesFor<NotifyInventoryChanges>(kAliceConn).empty(),
          "a rejected second take is not broadcast as if it succeeded");

    // Stale ownership epoch (0 or wrong epoch) is rejected
    Harness::ClearOutbox();
    RequestActorValueChanges staleAv{};
    staleAv.Id = npcServerId;
    staleAv.OwnershipEpoch = initialEpoch + 77;
    staleAv.Values[24] = 10.f;
    Check(DeliverWire(world, staleAv, pAlice), "stale-epoch actor value update deserializes cleanly");
    Check(DecodeOutboxMessagesFor<NotifyActorValueChanges>(kBobConn).empty(),
          "stale-epoch actor value update is rejected by server");

    // Non-owner update is rejected
    Harness::ClearOutbox();
    RequestActorValueChanges nonOwnerAv{};
    nonOwnerAv.Id = npcServerId;
    nonOwnerAv.OwnershipEpoch = initialEpoch;
    nonOwnerAv.Values[24] = 5.f;
    Check(DeliverWire(world, nonOwnerAv, pBob), "non-owner actor value update deserializes cleanly");
    Check(DecodeOutboxMessagesFor<NotifyActorValueChanges>(kAliceConn).empty(),
          "non-owner actor value update is rejected by server");

    // Phase 0 stability guard: NaN / Inf / kInfamy (61) actor values & non-finite DeltaHealth
    Harness::ClearOutbox();
    RequestActorValueChanges poisonAv{};
    poisonAv.Id = npcServerId;
    poisonAv.OwnershipEpoch = initialEpoch;
    poisonAv.Values[24] = std::numeric_limits<float>::quiet_NaN();
    poisonAv.Values[25] = std::numeric_limits<float>::infinity();
    poisonAv.Values[61] = 42.f; // kInfamy
    Check(DeliverWire(world, poisonAv, pAlice), "poison actor value packet delivers safely");
    Check(DecodeOutboxMessagesFor<NotifyActorValueChanges>(kBobConn).empty(),
          "NaN/Inf/kInfamy-only actor value packet is filtered out");
    const auto& storedAv = world.get<ActorValuesComponent>(npcEntity).CurrentActorValues.ActorValuesList;
    Check(std::fabs(storedAv.at(24) - 120.f) < 0.01f && storedAv.count(61) == 0,
          "server actor values remain uncorrupted (health still 120, kInfamy absent)");

    Harness::ClearOutbox();
    RequestHealthChangeBroadcast nanDmg{};
    nanDmg.Id = npcServerId;
    nanDmg.OwnershipEpoch = initialEpoch;
    nanDmg.Sequence = 4;
    nanDmg.DeltaHealth = std::numeric_limits<float>::quiet_NaN();
    Check(DeliverWire(world, nanDmg, pBob), "NaN health delta delivers safely");
    Check(DecodeOutboxMessagesFor<NotifyHealthChangeBroadcast>(kAliceConn).empty(),
          "NaN health delta is dropped before mutating health or broadcasting");

    // Movement relay
    Harness::ClearOutbox();
    ClientReferencesMoveRequest moveReq{};
    moveReq.Tick = 42;
    auto& upd = moveReq.Updates[npcServerId];
    upd.OwnershipEpoch = initialEpoch;
    upd.CellId = kBanneredMare;
    upd.Position.x = 135.f;
    upd.Position.y = 240.f;
    upd.Position.z = 5.f;
    Check(DeliverWire(world, moveReq, pAlice), "Alice sends movement update for owned NPC");
    const auto moveNotifies = DecodeOutboxMessagesFor<ServerReferencesMoveRequest>(kBobConn);
    Check(moveNotifies.size() == 1 && moveNotifies[0].Updates.count(npcServerId) == 1,
          "Bob receives relayed ServerReferencesMoveRequest for NPC");

    const auto avatar = *pAlice->GetCharacter();
    RequestOwnershipTransfer releaseAvatar{};
    releaseAvatar.ServerId = World::ToInteger(avatar);
    releaseAvatar.OwnershipEpoch = world.get<OwnerComponent>(avatar).OwnershipEpoch;
    DeliverWire(world, releaseAvatar, pAlice);
    Check(world.valid(avatar) && world.get<OwnerComponent>(avatar).GetOwner() == pAlice,
          "owner cannot relinquish its player avatar into remote ownership");

    // Ownership transfer on disconnect hands NPC to Bob with incremented epoch
    Harness::ClearOutbox();
    Disconnect(server, kAliceConn);
    const auto& ownerAfter = world.get<OwnerComponent>(npcEntity);
    Check(ownerAfter.GetOwner() == pBob && ownerAfter.OwnershipEpoch > initialEpoch,
          "disconnecting Alice transfers NPC ownership to Bob with a higher OwnershipEpoch");

    // Death state from new owner with new epoch succeeds
    Harness::ClearOutbox();
    RequestDeathStateChange deathReq{};
    deathReq.Id = npcServerId;
    deathReq.OwnershipEpoch = ownerAfter.OwnershipEpoch;
    deathReq.IsDead = true;
    Check(DeliverWire(world, deathReq, pBob), "Bob marks newly-owned NPC dead with new epoch");
    Check(world.get<CharacterComponent>(npcEntity).IsDead(), "NPC death state is recorded on server");

    Disconnect(server, kBobConn);
}

void RunOverlayChatSafetyScenario(Console::ConsoleRegistry& aConsole)
{
    std::printf("[scenario 3] OverlayService chat linear HTML stripping and pre-character safety\n");
    GameServer server(aConsole);
    auto& world = server.GetWorld();

    // Player created BEFORE character assignment (GetCharacter() == std::nullopt) must not crash on chat
    Player* pPreSpawn = world.GetPlayerManager().Create(3001);
    pPreSpawn->SetUsername("<b>Hero</b>");
    Check(!pPreSpawn->GetCharacter().has_value(), "pre-spawn player has no character yet");

    Harness::ClearOutbox();
    SendChatMessageRequest chatReq{};
    chatReq.MessageType = kGlobalChat;
    chatReq.ChatMessage = "<script>alert(1)</script>   <div>Hello</div> " + std::string(2048, '<');
    Check(DeliverWire(world, chatReq, pPreSpawn), "chat from pre-spawn player with hostile '<' payload succeeds");

    const auto broadcasts = DecodeOutboxMessagesFor<NotifyChatMessageBroadcast>(3001);
    Check(broadcasts.size() == 1, "global chat broadcast emitted once");
    if (!broadcasts.empty())
    {
        Check(std::string(broadcasts[0].PlayerName.c_str()) == "Hero", "HTML stripped from player name");
        const std::string body(broadcasts[0].ChatMessage.c_str());
        Check(body.find("<script>") == std::string::npos && body.find("Hello") != std::string::npos,
              "HTML tags stripped linearly without regex DoS while preserving message text");
    }

    Disconnect(server, 3001);
}

void RunNegotiatedCoopScenario(Console::ConsoleRegistry& aConsole)
{
    std::printf("[scenario 4] negotiated RFAB downed/revive, session fencing and legacy bypass rejection\n");
    GameServer server(aConsole);
    auto& world = server.GetWorld();
    auto& coop = world.ctx().at<CoopService>();
    constexpr TiltedPhoques::ConnectionId_t aliceConn = 4001, bobConn = 4002;
    const GameId cell(0, 0x165A8);
    Player* alice = JoinPlayer(world, aliceConn, "Alice", cell, 401);
    Player* bob = JoinPlayer(world, bobConn, "Bob", cell, 402);
    if (!alice || !bob || !alice->GetCharacter() || !bob->GetCharacter())
    {
        Check(false, "both negotiated clients have an authenticated character");
        return;
    }
    PartyCreateRequest create; DeliverWire(world, create, alice);
    PartyInviteRequest invite; invite.PlayerId = bob->GetId(); DeliverWire(world, invite, alice);
    PartyAcceptInviteRequest accept; accept.InviterId = alice->GetId(); DeliverWire(world, accept, bob);
    const auto ae = *alice->GetCharacter(), be = *bob->GetCharacter();
    const auto aid = World::ToInteger(ae), bid = World::ToInteger(be);
    const auto initialEpoch = world.get<OwnerComponent>(be).OwnershipEpoch;
    uint64_t aOperation = 0, bOperation = 0;
    auto send = [&](Player* player, Coop::Body body, uint64_t session = 0, uint64_t generation = 0) {
        RequestCoopAction request;
        request.Data.Session = session; request.Data.Generation = generation;
        request.Data.Operation = player == alice ? ++aOperation : ++bOperation;
        request.Data.Content = std::move(body);
        return DeliverWire(world, request, player);
    };
    Harness::ClearOutbox();
    send(bob, Coop::Presence{}, coop.Session(), coop.Generation());
    Check(!coop.Manages(bid), "presence before capability negotiation cannot enroll a character");
    send(alice, Coop::Hello{Coop::kProtocol, Coop::NativeDowned | Coop::GroundedAnchors, "alice-token", "se-1.5.97"});
    send(bob, Coop::Hello{Coop::kProtocol, Coop::NativeDowned | Coop::GroundedAnchors, "bob-token", "se-1.5.97"});
    Check(coop.Manages(aid) && coop.Manages(bid), "authenticated Hello enrolls the party's own characters");
    const auto welcome = DecodeOutboxMessagesFor<NotifyCoopAction>(bobConn);
    Check(std::any_of(welcome.begin(), welcome.end(), [&](const auto& m) {
        const auto* w = std::get_if<Coop::Welcome>(&m.Data.Content);
        return w && w->Error.empty() && m.Data.Session == coop.Session() && (w->Capabilities & Coop::NativeDowned);
    }), "Welcome returns the server session and only implemented capabilities");
    // Bob must be near Alice, but never at an invented reset offset.
    world.get<MovementComponent>(be).Position.x = 196.f;
    const auto base = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()) + 1000;
    auto presence = [&](Player* player, uint32_t flags, uint32_t revision) {
        const auto entity = *player->GetCharacter();
        const auto& position = world.get<MovementComponent>(entity).Position;
        Coop::Presence p;
        p.Flags = flags; p.DownedRevision = revision; p.Location.Cell = cell;
        p.Location.X = position.x; p.Location.Y = position.y; p.Location.Z = position.z;
        return p;
    };
    const auto session = coop.Session(), generation = coop.Generation();
    coop.Tick(base);
    send(alice, presence(alice, Coop::CanInteract | Coop::InCombat, 0), session, generation);
    Harness::ClearOutbox();
    send(bob, presence(bob, Coop::Bleedout | Coop::InCombat, 0), session + 1, generation);
    Check(!coop.IsDowned(bid), "a presence from another server session is rejected");
    send(bob, presence(bob, Coop::Bleedout | Coop::InCombat, 0), session, generation + 1);
    Check(!coop.IsDowned(bid), "a presence from another save generation is rejected");
    send(bob, presence(bob, Coop::Bleedout | Coop::InCombat, 0), session, generation);
    Check(coop.IsDowned(bid), "native bleedout becomes a server-latched downed state");
    Check(world.get<OwnerComponent>(be).OwnershipEpoch > initialEpoch &&
              world.get<ActorValuesComponent>(be).CurrentActorValues.ActorValuesList.at(24) == 0.f,
          "downed fences old HP/actions/movement with a new authority epoch");
    const auto downedEpoch = world.get<OwnerComponent>(be).OwnershipEpoch;
    RequestDeathStateChange kill;
    kill.Id = bid; kill.OwnershipEpoch = downedEpoch; kill.IsDead = true;
    DeliverWire(world, kill, bob);
    Check(!world.get<CharacterComponent>(be).IsDead(), "legacy death messages cannot kill a negotiated avatar");
    RequestHealthChangeBroadcast heal;
    heal.Id = bid; heal.OwnershipEpoch = downedEpoch; heal.Sequence = 1; heal.DeltaHealth = 100.f;
    DeliverWire(world, heal, bob);
    Check(world.get<ActorValuesComponent>(be).CurrentActorValues.ActorValuesList.at(24) == 0.f,
          "ordinary regeneration cannot bypass the downed latch");
    Coop::Revive hold{bid, 1, 1, true};
    send(alice, hold, session, generation);
    for (uint64_t elapsed = 250; elapsed <= 5000; elapsed += 250)
    {
        coop.Tick(base + elapsed);
        send(alice, presence(alice, Coop::CanInteract | Coop::InCombat, 0), session, generation);
        send(bob, presence(bob, Coop::Bleedout | Coop::InCombat, 1), session, generation);
        send(alice, hold, session, generation);
        if (elapsed < 5000)
            Check(coop.IsDowned(bid), "revive cannot complete before five server seconds");
    }
    Check(!coop.IsDowned(bid), "an uninterrupted hold revives at five server seconds");
    Check(world.get<OwnerComponent>(be).OwnershipEpoch > downedEpoch &&
              world.get<ActorValuesComponent>(be).CurrentActorValues.ActorValuesList.at(24) == 25.f,
          "revive restores 25 percent HP at a new epoch without resurrect/reset");
    Harness::ClearOutbox();
    ClientReferencesMoveRequest staleMove; staleMove.Tick = 500;
    auto& movement = staleMove.Updates[bid]; movement.OwnershipEpoch = downedEpoch;
    movement.CellId = cell; movement.Position.x = 999.f;
    DeliverWire(world, staleMove, bob);
    Check(world.get<MovementComponent>(be).Position.x == 196.f,
          "a pre-revive unreliable movement packet cannot move the recovered avatar");
    send(bob, presence(bob, Coop::Bleedout | Coop::InCombat, 1), session, generation);
    Check(!coop.IsDowned(bid), "old native bleedout samples cannot undo an acknowledged revive");
    Disconnect(server, bobConn);
    Disconnect(server, aliceConn);
}
void RunTransactionalInventoryScenario(Console::ConsoleRegistry& console)
{
    std::printf("[scenario 5] paired inventory reservation, exact retries, stale snapshots and reconnect\n");
    TemporaryWorkingDirectory fixture;
    Inventory received;
    Inventory::Entry item;
    item.BaseId = GameId(0, 0x12eb7); item.Count = 1;
    item.ExtraTextDisplayName = "One named depleted blade";
    item.ExtraPresence = Inventory::Entry::Charge | Inventory::Entry::Health | Inventory::Entry::Name;
    item.ExtraHealth = 1.25f;
    auto latestReply = [](TiltedPhoques::ConnectionId_t connection) -> Coop::TransferReply {
        const auto messages = DecodeOutboxMessagesFor<NotifyCoopAction>(connection);
        for (auto i = messages.rbegin(); i != messages.rend(); ++i)
            if (const auto* reply = std::get_if<Coop::TransferReply>(&i->Data.Content)) return *reply;
        return {};
    };
    {
        GameServer server(console);
        auto& world = server.GetWorld();
        world.ctx().at<ModsComponent>().AddStandard("Skyrim.esm");
        auto& coop = world.ctx().at<CoopService>();
        const GameId cell(0, 0x165A8);
        constexpr TiltedPhoques::ConnectionId_t ac = 5001, bc = 5002;
        auto* alice = JoinPlayer(world, ac, "Alice", cell, 501);
        auto* bob = JoinPlayer(world, bc, "Bob", cell, 502);
        Check(alice && bob && alice->GetCharacter() && bob->GetCharacter(), "transactional participants have assigned native actors");
        if (!alice || !bob || !alice->GetCharacter() || !bob->GetCharacter()) return;
        PartyCreateRequest create; DeliverWire(world, create, alice);
        PartyInviteRequest invite; invite.PlayerId = bob->GetId(); DeliverWire(world, invite, alice);
        PartyAcceptInviteRequest accept; accept.InviterId = alice->GetId(); DeliverWire(world, accept, bob);
        uint64_t aSeq{}, bSeq{};
        auto packet = [&](Player* player, Coop::Body body, bool hello = false) {
            RequestCoopAction request;
            request.Data.Session = hello ? 0 : coop.Session(); request.Data.Generation = hello ? 0 : coop.Generation();
            request.Data.Operation = player == alice ? ++aSeq : ++bSeq;
            request.Data.Content = std::move(body);
            return request;
        };
        auto ah = packet(alice, Coop::Hello{Coop::kProtocol, Coop::NativeDowned | Coop::TransactionalLoot, "wire-alice", "verified-test"}, true);
        auto bh = packet(bob, Coop::Hello{Coop::kProtocol, Coop::NativeDowned | Coop::TransactionalLoot, "wire-bob", "verified-test"}, true);
        DeliverWire(world, ah, alice); DeliverWire(world, bh, bob);
        Check(coop.UsesTransactionalInventory(alice) && coop.UsesTransactionalInventory(bob), "durable negotiation is attached to both authenticated bags");
        const uint32_t aid = World::ToInteger(*alice->GetCharacter()), bid = World::ToInteger(*bob->GetCharacter());
        const auto ae = *alice->GetCharacter(), be = *bob->GetCharacter();
        AssignObjectsRequest assign;
        ObjectData chest; chest.Id = GameId(0, 0xABC12); chest.CellId = cell; chest.CurrentInventory.Entries.push_back(item);
        assign.Objects.push_back(chest);
        Harness::ClearOutbox();
        Check(DeliverWire(world, assign, alice), "ordinary container is registered on access over the wire");
        const auto assigned = DecodeOutboxMessagesFor<AssignObjectsResponse>(ac);
        Check(assigned.size() == 1 && assigned.front().Objects.size() == 1, "canonical container assignment is returned");
        if (assigned.empty() || assigned.front().Objects.empty()) return;
        const auto sid = assigned.front().Objects.front().ServerId;
        const auto se = static_cast<entt::entity>(sid);
        Coop::Transfer take;
        take.Source = sid; take.Destination = aid;
        take.SourceRevision = world.get<InventoryComponent>(se).Content.Revision;
        take.DestinationRevision = world.get<InventoryComponent>(ae).Content.Revision;
        take.DestinationEpoch = world.get<OwnerComponent>(ae).OwnershipEpoch;
        take.Nonce = 41; take.Item = item;
        const auto first = packet(alice, take);
        Harness::ClearOutbox(); DeliverWire(world, first, alice);
        const auto prepared = latestReply(ac);
        Check(prepared.Result == Coop::TransferResult::Prepared && prepared.Operation != 0, "Prepared is issued only after a durable paired reservation");
        Check(Coop::ExactCount(world.get<InventoryComponent>(se).Content, item) == 1 &&
            Coop::ExactCount(world.get<InventoryComponent>(ae).Content, item) == 0, "neither endpoint moves before the native receipt");
        Harness::ClearOutbox(); DeliverWire(world, first, alice);
        Check(latestReply(ac).Operation == prepared.Operation && latestReply(ac).Result == Coop::TransferResult::Prepared,
            "an exact SAME-envelope retry reaches durable nonce replay instead of being silently dropped");
        auto collision = first; std::get<Coop::Transfer>(collision.Data.Content).Item.Count = 2;
        Harness::ClearOutbox(); DeliverWire(world, collision, alice);
        Check(DecodeOutboxMessagesFor<NotifyCoopAction>(ac).empty(), "same envelope sequence with different bytes is rejected");
        auto losing = take; losing.Destination = bid; losing.DestinationRevision = world.get<InventoryComponent>(be).Content.Revision;
        losing.DestinationEpoch = world.get<OwnerComponent>(be).OwnershipEpoch;
        auto other = packet(bob, losing);
        Harness::ClearOutbox(); DeliverWire(world, other, bob);
        Check(latestReply(bc).Result == Coop::TransferResult::Conflict, "competing taker cannot reserve the same item or either locked endpoint");
        RequestInventoryChanges bypass; bypass.ServerId = sid; bypass.Item = item; bypass.Item.Count = -1; bypass.UpdateClients = true;
        DeliverWire(world, bypass, bob);
        Check(Coop::ExactCount(world.get<InventoryComponent>(se).Content, item) == 1, "unilateral object delta cannot bypass a transfer reservation");
        const auto receipt = packet(alice, Coop::TransferReceipt{prepared.Operation, true, false});
        Harness::ClearOutbox(); DeliverWire(world, receipt, alice);
        Check(latestReply(ac).Result == Coop::TransferResult::Committed, "native applied receipt durably commits both endpoints");
        Check(Coop::ExactCount(world.get<InventoryComponent>(se).Content, item) == 0 &&
            Coop::ExactCount(world.get<InventoryComponent>(ae).Content, item) == 1 &&
            Coop::ExactCount(world.get<InventoryComponent>(be).Content, item) == 0, "one source item has exactly one recipient with all zero-valued extras intact");
        const auto revision = world.get<InventoryComponent>(ae).Content.Revision;
        Harness::ClearOutbox(); DeliverWire(world, receipt, alice);
        Check(latestReply(ac).Result == Coop::TransferResult::Committed && world.get<InventoryComponent>(ae).Content.Revision == revision,
            "duplicate native receipt does not move or increment either endpoint again");
        RequestInventorySnapshot stale; stale.ServerId = aid; stale.OwnershipEpoch = take.DestinationEpoch;
        stale.CurrentInventory.Revision = take.DestinationRevision;
        DeliverWire(world, stale, alice);
        Check(Coop::ExactCount(world.get<InventoryComponent>(ae).Content, item) == 1, "pre-transfer owner snapshot cannot erase the committed recipient item");
        Harness::ClearOutbox(); DeliverWire(world, first, alice);
        Check(latestReply(ac).Result == Coop::TransferResult::Committed, "a late request retry returns Committed, never a fresh native execution");
        received = world.get<InventoryComponent>(ae).Content;
        Disconnect(server, ac);
        alice = JoinPlayer(world, ac + 10, "Alice reconnect", cell, 511, &received);
        aSeq = 0;
        auto reconnect = packet(alice, Coop::Hello{Coop::kProtocol, Coop::NativeDowned | Coop::TransactionalLoot, "wire-alice", "verified-test"}, true);
        DeliverWire(world, reconnect, alice);
        Check(coop.UsesTransactionalInventory(alice), "reconnect binds the same saved character and exact live bag, not a new empty endpoint");
        Disconnect(server, ac + 10); Disconnect(server, bc);
    }
    {
        GameServer server(console);
        auto& world = server.GetWorld();
        world.ctx().at<ModsComponent>().AddStandard("Skyrim.esm");
        auto& coop = world.ctx().at<CoopService>();
        auto* alice = JoinPlayer(world, 5201, "Alice restarted", GameId(0, 0x165A8), 521, &received);
        RequestCoopAction hello;
        hello.Data.Operation = 1;
        hello.Data.Content = Coop::Hello{Coop::kProtocol, Coop::NativeDowned | Coop::TransactionalLoot, "wire-alice", "verified-test"};
        DeliverWire(world, hello, alice);
        Check(coop.UsesTransactionalInventory(alice), "server restart restores canonical named-item receipt by plugin identity");
        Disconnect(server, 5201);
        auto* rolledBack = JoinPlayer(world, 5202, "Alice old save", GameId(0, 0x165A8), 522);
        Harness::ClearOutbox(); DeliverWire(world, hello, rolledBack);
        Check(!coop.UsesTransactionalInventory(rolledBack), "old empty ESS bag cannot silently overwrite a post-transfer durable campaign");
        const auto messages = DecodeOutboxMessagesFor<NotifyCoopAction>(5202);
        Check(std::any_of(messages.begin(), messages.end(), [](const auto& m) { const auto* w = std::get_if<Coop::Welcome>(&m.Data.Content); return w && !w->Error.empty(); }),
            "save rollback mismatch has an explicit refusal instead of guessed AddItem compensation");
        Disconnect(server, 5202);
    }
}
} // namespace

int main()
{
    auto conOut = spdlog::stdout_color_mt("ConOut");
    conOut->set_level(spdlog::level::err);
    spdlog::set_default_logger(conOut);
    spdlog::set_level(spdlog::level::err);

    TemporaryWorkingDirectory workdir;
    Console::ConsoleRegistry console("ConOut");

    RunEntityZeroAndLifecycleScenario(console);
    RunTwoPlayerSyncAndOwnershipScenario(console);
    RunOverlayChatSafetyScenario(console);
    RunNegotiatedCoopScenario(console);
    RunTransactionalInventoryScenario(console);

    if (g_failures != 0)
    {
        std::printf("\n[RFAB_RESULT] server harness FAILED (%d failure(s))\n", g_failures);
        return 1;
    }

    std::printf("\n[RFAB_RESULT] server harness PASSED (all production-wire scenarios ok)\n");
    return 0;
}
