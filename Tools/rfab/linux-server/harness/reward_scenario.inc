// Real server services + both production codecs. This does NOT stand in for an in-game RFAB
// adapter: personal pool mutation is represented by receipts and tested separately at its boundary.
void RunSemanticRewardScenario(Console::ConsoleRegistry& console)
{
    std::printf("[scenario 8] semantic XP, frozen base/cohort, death authority, personal receipts and restart\n");
    TemporaryWorkingDirectory fixture;
    constexpr TiltedPhoques::ConnectionId_t ac = 8001, bc = 8002;
    constexpr uint64_t signature = 0x1234432156788765ull;
    const GameId cell(0, 0x165A8);
    uint64_t revision{}, digest{};
    const auto hello = [&](const char* character) {
        Coop::Hello h{Coop::kProtocol, Coop::NativeDowned | Coop::SemanticRewards | Coop::TransactionalLoot,
            character, "test-dual-pool"};
        h.RewardSignature = signature; h.RewardKinds = 15;
        return h;
    };
    const auto offers = [](TiltedPhoques::ConnectionId_t conn) {
        std::vector<Coop::RewardGrant> out;
        for (const auto& message : DecodeOutboxMessagesFor<NotifyCoopAction>(conn))
            if (const auto* grant = std::get_if<Coop::RewardGrant>(&message.Data.Content)) out.push_back(*grant);
        return out;
    };
    const auto reports = [](TiltedPhoques::ConnectionId_t conn) {
        std::vector<Coop::RewardReport> out;
        for (const auto& message : DecodeOutboxMessagesFor<NotifyCoopAction>(conn))
            if (const auto* report = std::get_if<Coop::RewardReport>(&message.Data.Content)) out.push_back(*report);
        return out;
    };
    {
        GameServer server(console);
        auto& world = server.GetWorld();
        auto& coop = world.ctx().at<CoopService>();
        auto* alice = JoinPlayer(world, ac, "XP Alice", cell, 801);
        auto* bob = JoinPlayer(world, bc, "XP Bob", cell, 802);
        if (!alice || !bob || !alice->GetCharacter() || !bob->GetCharacter())
        { Check(false, "semantic participants have authenticated own avatars"); return; }
        PartyCreateRequest create; DeliverWire(world, create, alice);
        PartyInviteRequest invite; invite.PlayerId = bob->GetId(); DeliverWire(world, invite, alice);
        PartyAcceptInviteRequest accept; accept.InviterId = alice->GetId(); DeliverWire(world, accept, bob);
        uint64_t ao{}, bo{}, now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()) + 10;
        auto send = [&](Player* player, Coop::Body body, bool greeting = false) {
            RequestCoopAction request;
            request.Data.Session = greeting ? 0 : coop.Session(); request.Data.Generation = greeting ? 0 : coop.Generation();
            request.Data.Operation = player == alice ? ++ao : ++bo; request.Data.Content = std::move(body);
            return DeliverWire(world, request, player);
        };
        send(alice, hello("xp-alice"), true);
        auto wrong = hello("xp-bob"); wrong.RewardSignature++;
        Harness::ClearOutbox(); send(bob, wrong, true);
        Check(coop.UsesSemanticRewards(alice) && !coop.UsesSemanticRewards(bob),
            "cohort cannot mix incompatible installed semantic XP profiles");
        send(bob, hello("xp-bob"), true);
        Check(coop.UsesSemanticRewards(bob), "same installed profile admits both independent personal XP receivers");
        const auto aid = World::ToInteger(*alice->GetCharacter()), bid = World::ToInteger(*bob->GetCharacter());
        const auto presence = [&](Player* player, uint32_t flags = Coop::CanInteract) {
            const auto& position = world.get<MovementComponent>(*player->GetCharacter()).Position;
            Coop::Presence p; p.Location.Cell = cell; p.Location.X = position.x; p.Location.Y = position.y; p.Location.Z = position.z;
            p.Flags = flags; send(player, p);
        };
        coop.Tick(now); presence(alice); presence(bob);
        Coop::Reward quest;
        quest.Kind = Coop::RewardKind::Quest; quest.Actor = aid; quest.Source = GameId(0, 0x12345);
        quest.Occurrence = 1; quest.BaseXP = 103; quest.Nonce = 1;
        SyncExperienceRequest legacy; legacy.Experience = 20.f;
        Harness::ClearOutbox(); Check(DeliverWire(world, legacy, alice), "legacy XP still crosses the real decoder");
        Check(DecodeOutboxMessagesFor<NotifySyncExperience>(bc).empty(), "semantic mode suppresses legacy redistribution instead of paying twice");
        Check(send(alice, quest), "base reward reaches the production semantic wire decoder");
        const auto a = offers(ac), b = offers(bc);
        if (a.empty() || b.empty()) { Check(false, "both nearby participants receive a durable offer"); return; }
        const auto first = a.back();
        Check(first.Result == Coop::RewardResult::Offered && first.RewardId == b.back().RewardId &&
            first.BaseXP == 103 && b.back().BaseXP == 103 && !first.BeforeRevision && !b.back().BeforeRevision,
            "one semantic event freezes the SAME BASE, not equal final wallets");
        const auto complete = [&](Player* player, const Coop::RewardGrant& grant, int32_t personal) {
            const auto next = Coop::AdvanceRewardDigest(grant.BeforeDigest, grant.RewardId, grant.BaseXP);
            send(player, Coop::RewardReceipt{grant.RewardId, true, false, personal, grant.BeforeRevision + 1, next});
            return next;
        };
        Harness::ClearOutbox(); digest = complete(alice, first, 125); revision = 1;
        complete(bob, b.back(), 205);
        Check(!offers(ac).empty() && offers(ac).back().Result == Coop::RewardResult::Committed &&
            !offers(bc).empty() && offers(bc).back().Result == Coop::RewardResult::Committed,
            "different personal multipliers/final XP commit independently without wallet normalization");
        auto duplicate = quest; duplicate.BaseXP = 600; duplicate.Nonce = 2; duplicate.Actor = bid;
        Harness::ClearOutbox(); send(bob, duplicate);
        const auto dup = offers(bc);
        Check(!dup.empty() && dup.back().RewardId == first.RewardId && dup.back().BaseXP == 103 &&
            dup.back().Result == Coop::RewardResult::Committed,
            "second report/changed amount after personal spending confirms the frozen event, never a new award");
        Harness::ClearOutbox(); complete(alice, first, 125);
        Check(!offers(ac).empty() && offers(ac).back().Result == Coop::RewardResult::Committed,
            "duplicate exact native receipt does not advance the semantic account twice");
        // Kill entitlement originates only in the accepted owner's alive -> dead transition.
        AssignCharacterRequest npc;
        npc.Cookie = 8101; npc.ReferenceId = GameId(0, 0x27041); npc.FormId = GameId(0, 0x37041); npc.CellId = cell;
        npc.Position.x = 120.f; npc.Position.y = 200.f;
        npc.CurrentActorData.InitialActorValues.ActorValuesList[24] = 0.f;
        npc.CurrentActorData.InitialActorValues.ActorMaxValuesList[24] = 150.f;
        Harness::ClearOutbox(); DeliverWire(world, npc, alice);
        const auto assignment = DecodeOutboxMessagesFor<AssignCharacterResponse>(ac);
        if (assignment.empty()) { Check(false, "kill source gets an authoritative NPC"); return; }
        const auto id = assignment.back().ServerId, epoch = assignment.back().OwnershipEpoch;
        Coop::Reward kill;
        kill.Kind = Coop::RewardKind::Kill; kill.Actor = id; kill.Source = npc.ReferenceId;
        kill.BaseXP = 100; kill.Occurrence = 1; kill.Incarnation = 999; kill.Nonce = 3;
        Harness::ClearOutbox(); send(alice, kill);
        const auto rejected = reports(ac);
        Check(!rejected.empty() && !rejected.back().Accepted && offers(ac).empty(),
            "client cannot invent a rewarded kill life just by supplying an NPC/base XP");
        RequestDeathStateChange death; death.Id = id; death.OwnershipEpoch = epoch; death.IsDead = true;
        Harness::ClearOutbox(); DeliverWire(world, death, bob);
        Check(DecodeOutboxMessagesFor<NotifyCoopAction>(ac).empty(), "non-owner death cannot mint an XP life token");
        Harness::ClearOutbox(); DeliverWire(world, death, alice);
        std::optional<Coop::RewardSource> source;
        for (const auto& message : DecodeOutboxMessagesFor<NotifyCoopAction>(ac))
            if (const auto* value = std::get_if<Coop::RewardSource>(&message.Data.Content)) source = *value;
        Check(source && source->Actor == id && source->Source == npc.ReferenceId && source->Incarnation,
            "accepted authoritative death publishes a reference + incarnation, not BaseId/time/wallet identity");
        if (!source) return;
        kill.Incarnation = source->Incarnation; kill.Nonce++;
        Harness::ClearOutbox(); send(alice, kill);
        const auto ka = offers(ac), kb = offers(bc);
        if (ka.empty() || kb.empty()) { Check(false, "death freezes both nearby recipients"); return; }
        Check(ka.back().BeforeRevision == 1 && kb.back().BeforeRevision == 1,
            "next semantic award starts from each participant's committed receipt chain");
        digest = complete(alice, ka.back(), 125); ++revision; complete(bob, kb.back(), 200);
        // Downed remains an eligible nearby teammate. Loading and distant do not.
        presence(bob, Coop::Bleedout | Coop::InCombat);
        Check(coop.IsDowned(bid), "reward scenario exercises an actual server-latched downed teammate");
        quest.Source = GameId(0, 0x12346); quest.Nonce = 5;
        Harness::ClearOutbox(); send(alice, quest);
        const auto da = offers(ac), db = offers(bc);
        if (da.empty() || db.empty()) { Check(false, "downed nearby teammate is still a base reward recipient"); return; }
        Check(db.back().RewardId == da.back().RewardId, "downed is not silently excluded from equal base rewards");
        digest = complete(alice, da.back(), 125); ++revision; complete(bob, db.back(), 200);
        presence(bob, Coop::Loading);
        quest.Source = GameId(0, 0x12347); quest.Nonce = 6;
        Harness::ClearOutbox(); send(alice, quest);
        const auto solo = offers(ac);
        Check(!solo.empty() && offers(bc).empty(), "loading peer is excluded before the recipient set is frozen");
        if (solo.empty()) return;
        digest = complete(alice, solo.back(), 125); ++revision;
        presence(bob, Coop::Bleedout);
        auto late = quest; late.Actor = bid; late.Nonce = 7;
        Harness::ClearOutbox(); send(bob, late);
        Check(offers(bc).empty() && !reports(bc).empty() && reports(bc).back().Accepted,
            "late duplicate report cannot extend a frozen recipient set or manufacture catch-up XP");
        Check(std::filesystem::is_regular_file("Data/rfab_campaign/rfab-coop.rewards"),
            "published semantic offers/commits have a real campaign journal");
        Disconnect(server, ac); Disconnect(server, bc);
    }
    {
        GameServer server(console);
        auto& world = server.GetWorld(); auto& coop = world.ctx().at<CoopService>();
        auto* alice = JoinPlayer(world, ac + 10, "XP Alice restart", cell, 820);
        if (!alice || !alice->GetCharacter()) { Check(false, "semantic restart avatar is assigned"); return; }
        RequestCoopAction request; request.Data.Operation = 1;
        request.Data.Content = hello("xp-alice");
        Harness::ClearOutbox(); DeliverWire(world, request, alice);
        Check(!coop.UsesSemanticRewards(alice), "older ESS/co-save reward cursor cannot erase the durable post-reward campaign chain");
        auto matching = hello("xp-alice"); matching.Rewards.push_back({"rfab-coop", revision, digest});
        request.Data.Content = matching; request.Data.Operation++;
        Harness::ClearOutbox(); DeliverWire(world, request, alice);
        Check(coop.UsesSemanticRewards(alice), "server restart admits an exactly matching semantic save receipt, not a new wallet");
        Check(offers(ac + 10).empty(), "restart/reconnect has no automatic replay of previously committed rewards");
        Disconnect(server, ac + 10);
    }
}
