// Included inside the main harness namespace, using the same production wire/outbox helpers.
void RunEnemyHealthScalingScenario(Console::ConsoleRegistry& console)
{
    std::printf("[scenario 7] save-imported per-life HP x2, eligibility, frozen cohort and owner handoff\n");
    TemporaryWorkingDirectory fixture;
    for (unsigned mode = 0; mode < 3; ++mode)
    {
        GameServer server(console);
        auto& world = server.GetWorld();
        auto& coop = world.ctx().at<CoopService>();
        constexpr TiltedPhoques::ConnectionId_t ac = 7001, bc = 7002;
        const GameId cell(0, 0x165A8);
        auto* alice = JoinPlayer(world, ac, "Scale Alice", cell, 701);
        auto* bob = JoinPlayer(world, bc, "Scale Bob", cell, 702);
        if (!alice || !bob || !alice->GetCharacter() || !bob->GetCharacter())
        { Check(false, "scaling clients have characters"); return; }
        PartyCreateRequest create; DeliverWire(world, create, alice);
        PartyInviteRequest invite; invite.PlayerId = bob->GetId(); DeliverWire(world, invite, alice);
        PartyAcceptInviteRequest accept; accept.InviterId = alice->GetId(); DeliverWire(world, accept, bob);
        uint64_t ao{}, bo{};
        auto send = [&](Player* p, Coop::Body body, bool hello = false) {
            RequestCoopAction request;
            request.Data.Session = hello ? 0 : coop.Session(); request.Data.Generation = hello ? 0 : coop.Generation();
            request.Data.Operation = p == alice ? ++ao : ++bo;
            request.Data.Content = std::move(body); return DeliverWire(world, request, p);
        };
        const auto capabilities = Coop::NativeDowned | Coop::TransactionalLoot | Coop::EnemyScaling;
        const auto aKey = "0000000000000000000000000000070" + std::to_string(mode);
        const auto bKey = "0000000000000000000000000000071" + std::to_string(mode);
        send(alice, Coop::Hello{Coop::kProtocol, capabilities, aKey, "native-scale-1"}, true);
        auto joinBob = [&] {
            send(bob, Coop::Hello{Coop::kProtocol, capabilities, bKey, "native-scale-1"}, true);
            const auto replies = DecodeOutboxMessagesFor<NotifyCoopAction>(bc);
            Check(std::any_of(replies.begin(), replies.end(), [](const auto& m) {
                const auto* w = std::get_if<Coop::Welcome>(&m.Data.Content);
                return w && w->Error.empty() && (w->Capabilities & Coop::EnemyScaling);
            }), "enemy-health capability requires the durable native inventory/save boundary");
        };
        if (mode != 1) joinBob();
        auto presence = [&](Player* p, bool combat) {
            const auto& position = world.get<MovementComponent>(*p->GetCharacter()).Position;
            Coop::Presence value;
            value.Location.Cell = cell; value.Location.X = position.x; value.Location.Y = position.y;
            value.Location.Z = position.z; value.Flags = combat ? Coop::InCombat : Coop::CanInteract;
            send(p, value);
        };
        const auto now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        coop.Tick(now + 1);
        presence(alice, false); if (mode != 1) presence(bob, false);
        presence(alice, true); // seals the encounter policy once, not when a new peer later appears
        if (mode == 1) { joinBob(); presence(bob, false); }
        Harness::ClearOutbox();
        AssignCharacterRequest npc;
        npc.Cookie = 7100; npc.ReferenceId = GameId(0, 0x27001); npc.FormId = GameId(0, 0x37001); npc.CellId = cell;
        npc.Position.x = 120.f; npc.Position.y = 220.f;
        auto& initial = npc.CurrentActorData.InitialActorValues;
        initial.ActorValuesList[24] = mode == 2 ? 220.f : 120.f;
        initial.ActorMaxValuesList[24] = mode == 2 ? 300.f : 150.f;
        if (mode == 2) initial.Scale = {77, 150.f, 300.f}; // prior matching ESS+SKSE native stamp
        Check(DeliverWire(world, npc, alice), "NPC assignment and optional saved scale pass the real codec");
        const auto assignments = DecodeOutboxMessagesFor<AssignCharacterResponse>(ac);
        if (assignments.empty()) { Check(false, "NPC gets its own assignment"); return; }
        const auto id = assignments.back().ServerId, epoch = assignments.back().OwnershipEpoch;
        const auto entity = static_cast<entt::entity>(id);
        auto& values = world.get<ActorValuesComponent>(entity).CurrentActorValues;
        RequestHealthChangeBroadcast damage;
        damage.Id = id; damage.OwnershipEpoch = epoch; damage.Sequence = 1; damage.DeltaHealth = -10.f;
        DeliverWire(world, damage, bob);
        const auto unscaledHealth = values.ActorValuesList.at(24);
        send(bob, Coop::EnemyContact{id, epoch, true, false, false});
        Check(values.ActorValuesList.at(24) == unscaledHealth && values.Scale == initial.Scale,
            "a non-owner contact cannot multiply the owner's enemy HP");
        Harness::ClearOutbox();
        send(alice, Coop::EnemyContact{id, epoch, true, false, false});
        const float expectedHealth = mode == 0 ? 220.f : unscaledHealth;
        const float expectedMax = mode == 1 ? 150.f : 300.f;
        Check(values.ActorValuesList.at(24) == expectedHealth && values.ActorMaxValuesList.at(24) == expectedMax,
            mode == 1 ? "late peer does not resize/heal an already active solo encounter" :
            mode == 2 ? "save-imported x2 life is never multiplied to x4" : "first scale doubles canonical max and fraction, not a full heal");
        const auto scale = values.Scale;
        const auto revision = values.HealthRevision;
        for (int retry = 0; retry < 4; ++retry) send(alice, Coop::EnemyContact{id, epoch, true, false, false});
        Check(values.Scale == scale && values.HealthRevision == revision && values.ActorValuesList.at(24) == expectedHealth,
            "repeated contacts/new packet sequences are not repeated multipliers");
        if (mode == 0)
        {
            const auto notices = DecodeOutboxMessagesFor<NotifyHealthChangeBroadcast>(bc);
            Check(std::any_of(notices.begin(), notices.end(), [&](const auto& n) {
                return n.Id == id && n.Scale == scale && n.Health == 220.f && n.SourcePlayerId == bob->GetId() && n.Sequence == 1;
            }), "scale snapshot includes this recipient's already applied damage watermark");
        }
        if (mode != 1)
        {
            RequestActorMaxValueChanges stale;
            stale.Id = id; stale.OwnershipEpoch = epoch; stale.Values[24] = 600.f;
            DeliverWire(world, stale, alice);
            stale.Values[24] = 150.f; DeliverWire(world, stale, alice);
            Check(values.ActorMaxValuesList.at(24) == 300.f && values.Scale == scale,
                "periodic max snapshots cannot undo x2 or multiply it again");
        }
        damage.DeltaHealth = -20.f; DeliverWire(world, damage, alice);
        Check(values.ActorValuesList.at(24) == expectedHealth - 20.f, "post-scale hits remain absolute signed damage");
        DeliverWire(world, damage, alice);
        Check(values.ActorValuesList.at(24) == expectedHealth - 20.f, "duplicate post-scale hit is acknowledged once");
        // Re-assignment by another viewer returns the authoritative life/max, not its stale local snapshot.
        Harness::ClearOutbox(); npc.Cookie = 7101; DeliverWire(world, npc, bob);
        const auto views = DecodeOutboxMessagesFor<AssignCharacterResponse>(bc);
        Check(!views.empty() && views.back().AllActorValues.Scale == scale &&
            views.back().AllActorValues.ActorMaxValuesList.at(24) == expectedMax &&
            views.back().AllActorValues.ActorValuesList.at(24) == expectedHealth - 20.f,
            "late viewer receives canonical life/base/max and damaged current HP");
        for (unsigned kind = 0; kind < 5; ++kind)
        {
            auto excluded = npc; excluded.Cookie = 7200 + kind; excluded.ReferenceId = GameId(0, 0x28001 + kind);
            excluded.CurrentActorData.InitialActorValues.Scale = {};
            excluded.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = 150.f;
            excluded.CurrentActorData.InitialActorValues.ActorValuesList[24] = 120.f;
            excluded.IsPlayerSummon = kind == 0; excluded.IsMount = kind == 1;
            Harness::ClearOutbox(); DeliverWire(world, excluded, alice);
            const auto result = DecodeOutboxMessagesFor<AssignCharacterResponse>(ac);
            if (result.empty()) { Check(false, "excluded NPC still has ordinary rendering/ownership"); continue; }
            send(alice, Coop::EnemyContact{result.back().ServerId, result.back().OwnershipEpoch, kind != 2, kind == 3, kind == 4});
            const auto& untouched = world.get<ActorValuesComponent>(static_cast<entt::entity>(result.back().ServerId)).CurrentActorValues;
            Check(!untouched.Scale.Life && untouched.ActorMaxValuesList.at(24) == 150.f,
                "summon/mount/nonhostile/ally/scripted actor is never automatically scaled");
        }
        if (scale.Life)
        {
            for (unsigned kind = 0; kind < 4; ++kind)
            {
                auto imported = npc; imported.Cookie = 7300 + kind;
                imported.ReferenceId = kind == 3 ? GameId(0xFFFFFFFFu, 0x1234) : GameId(0, 0x29001 + kind);
                imported.CurrentActorData.InitialActorValues.Scale = scale;
                imported.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = scale.Maximum;
                imported.IsPlayerSummon = kind == 1; imported.IsMount = kind == 2;
                Harness::ClearOutbox(); DeliverWire(world, imported, alice);
                Check(DecodeOutboxMessagesFor<AssignCharacterResponse>(ac).empty(),
                    "one saved life cannot be imported onto a different ref, summon, mount or temporary clone");
            }
        }
        const auto avatar = *alice->GetCharacter();
        send(alice, Coop::EnemyContact{World::ToInteger(avatar), world.get<OwnerComponent>(avatar).OwnershipEpoch, true, false, false});
        Check(!world.get<ActorValuesComponent>(avatar).CurrentActorValues.Scale.Life, "a forged enemy contact cannot scale the own avatar");
        Harness::ClearOutbox(); Disconnect(server, ac);
        const auto& owner = world.get<OwnerComponent>(entity);
        const auto grants = DecodeOutboxMessagesFor<NotifyOwnershipTransfer>(bc);
        Check(owner.GetOwner() == bob && owner.OwnershipEpoch > epoch && values.Scale == scale &&
            values.ActorMaxValuesList.at(24) == expectedMax && values.ActorValuesList.at(24) == expectedHealth - 20.f,
            "owner disconnect/handoff preserves absolute damaged HP, max and life without healing");
        Check(std::any_of(grants.begin(), grants.end(), [&](const auto& grant) {
            return grant.ServerId == id && grant.CurrentActorData.InitialActorValues.Scale == scale;
        }), "ownership control snapshot carries the same saved-life stamp");
        Disconnect(server, bc);
    }
}
