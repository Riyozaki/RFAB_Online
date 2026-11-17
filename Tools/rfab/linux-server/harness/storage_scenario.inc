// Included in the harness anonymous namespace; exercises the shipped services and codec.
void RunPersistentStorageScenario(Console::ConsoleRegistry& console)
{
    std::printf("[scenario 6] hidden persistent storage from two access cells, explicit import and restart\n");
    TemporaryWorkingDirectory fixture;
    std::error_code ec;
    std::filesystem::create_directories("config", ec);
    const std::string rules = "RFAB.esp\t801\t0\tRFAB.esp\t800\t0\tSkyrim.esm\t165a8\t0\t-\t0\t0\t100\t200\t0\n"
        "RFAB.esp\t802\t0\tRFAB.esp\t800\t0\tSkyrim.esm\t16789\t0\t-\t0\t0\t100\t200\t0\n"
        "RFAB.esp\t803\t0\tRFAB.esp\t800\t0\tSkyrim.esm\t165a8\t0\t-\t0\t0\t10000\t200\t0\n";
    { std::ofstream out("config/RFAB_Storage.tsv"); out << rules; }
    Coop::StorageCatalog catalog;
    Check(catalog.Parse(rules), "test allowlist is canonical and both access points resolve one backing");
    Inventory::Entry item;
    item.BaseId = GameId(1, 0x900); item.Count = 1; item.ExtraTextDisplayName = "Private imported depleted relic";
    item.ExtraPresence = Inventory::Entry::Name | Inventory::Entry::Charge;
    const GameId cityA(0, 0x165A8), cityB(0, 0x16789), accessA(1, 0x801), accessB(1, 0x802), backing(1, 0x800);
    const auto latest = []<class T>(TiltedPhoques::ConnectionId_t connection) -> std::optional<T> {
        const auto messages = DecodeOutboxMessagesFor<NotifyCoopAction>(connection);
        for (auto it = messages.rbegin(); it != messages.rend(); ++it)
            if (const auto* body = std::get_if<T>(&it->Data.Content)) return *body;
        return {};
    };
    {
        GameServer server(console);
        auto& world = server.GetWorld(); auto& coop = world.ctx().at<CoopService>();
        auto& mods = world.ctx().at<ModsComponent>();
        Check(mods.AddStandard("Skyrim.esm") == 0 && mods.AddStandard("RFAB.esp") == 1, "profile's two standard plugins negotiate their actual dictionary ids");
        Check(!mods.Find("missing.esp", false) && !mods.Find("RFAB.esp", true), "read-only resolver refuses missing plugin and standard/light mismatch");
        Check(mods.AddLite("RFAB.esp") == 0xFFFFFFFEu, "same filename cannot silently become a light plugin after negotiation");
        auto* alice = JoinPlayer(world, 6001, "Alice bank city A", cityA, 601);
        auto* bob = JoinPlayer(world, 6002, "Bob bank city B", cityB, 602);
        if (!alice || !bob || !alice->GetCharacter() || !bob->GetCharacter()) { Check(false, "bank players assigned"); return; }
        alice->SetModIds({0, 1}); bob->SetModIds({0, 1});
        uint64_t aSeq{}, bSeq{};
        const auto send = [&](Player* player, Coop::Body body, bool hello = false) {
            RequestCoopAction message;
            message.Data.Session = hello ? 0 : coop.Session(); message.Data.Generation = hello ? 0 : coop.Generation();
            message.Data.Operation = player == alice ? ++aSeq : ++bSeq; message.Data.Content = std::move(body);
            return DeliverWire(world, message, player);
        };
        const uint32_t capabilities = Coop::NativeDowned | Coop::TransactionalLoot | Coop::PersistentStorage;
        send(alice, Coop::Hello{Coop::kProtocol, capabilities, "bank-alice", "verified-profile", catalog.Signature()}, true);
        send(bob, Coop::Hello{Coop::kProtocol, capabilities, "bank-bob", "wrong-profile", catalog.Signature() ^ 1}, true);
        Check(!coop.UsesTransactionalInventory(bob), "mismatched installed catalog refuses the handshake before joining or importing a bag");
        send(bob, Coop::Hello{Coop::kProtocol, capabilities, "bank-bob", "verified-profile", catalog.Signature()}, true);
        Check(coop.UsesTransactionalInventory(alice) && coop.UsesTransactionalInventory(bob), "distinct cohorts in different cells can negotiate one campaign bank");
        const auto assignAccess = [&](Player* player, GameId point, GameId cell) {
            AssignObjectsRequest message; ObjectData data; data.Id = point; data.CellId = cell;
            message.Objects.push_back(data); return DeliverWire(world, message, player);
        };
        Check(assignAccess(alice, accessA, cityA) && assignAccess(bob, accessB, cityB), "only the visible access references are cell-registered; the hidden backing is not");
        Coop::StorageOpen openA{"RFAB.esp", 0x800, backing, accessA, false, {}};
        auto openB = openA; openB.AccessPoint = accessB;
        Harness::ClearOutbox(); send(alice, openA); send(bob, openB);
        auto as = latest.template operator()<Coop::StorageState>(6001), bs = latest.template operator()<Coop::StorageState>(6002);
        Check(as && bs && as->NeedsImport && bs->NeedsImport && as->Items.empty() && bs->Items.empty(), "old personal storage is never imported merely because somebody opened a menu");
        if (!as || !bs) return;
        // Point803 IS catalog-listed and claims the right cell, but its installed DATA is far
        // away. AssignObjectsRequest has no trusted spatial authority to move it near Alice.
        assignAccess(alice, GameId(1, 0x803), cityA);
        auto distant = openA; distant.AccessPoint = GameId(1, 0x803);
        Harness::ClearOutbox(); send(alice, distant);
        Check(!latest.template operator()<Coop::StorageState>(6001),
            "registered same-cell catalog point cannot open a bank from its distant static coordinates");
        auto forged = openA; forged.ConfirmImport = true;
        auto foreign = item; foreign.ExtraOwnerId = GameId(54, 0x123); foreign.ExtraPresence |= Inventory::Entry::Ownership;
        forged.Import.push_back(foreign);
        Harness::ClearOutbox(); send(alice, forged); send(alice, openA);
        const auto afterForged = latest.template operator()<Coop::StorageState>(6001);
        Check(afterForged && afterForged->NeedsImport && afterForged->Items.empty(),
            "a forged import's absent extra-data plugin is refused before any durable bank mutation/fault");
        entt::entity accessEntity = entt::null;
        for (auto e : world.view<FormIdComponent, ObjectComponent>())
            if (world.get<FormIdComponent>(e).Id == accessA) accessEntity = e;
        Check(accessEntity != entt::null, "visible access entity exists for current-lock revalidation");
        if (accessEntity == entt::null) return;
        world.get<ObjectComponent>(accessEntity).CurrentLockData.IsLocked = true;
        Harness::ClearOutbox(); send(alice, openA);
        Check(!latest.template operator()<Coop::StorageState>(6001), "a currently locked visible access cannot read or initialize the hidden bank");
        world.get<ObjectComponent>(accessEntity).CurrentLockData.IsLocked = false;
        Check(as->Entity == bs->Entity && as->Key == bs->Key && as->Backing == backing,
            "two cities address one synthetic durable backing, independent of off-cell distance and temporary object ids");
        openA.ConfirmImport = true; openA.Import.push_back(item);
        Harness::ClearOutbox(); send(alice, openA);
        as = latest.template operator()<Coop::StorageState>(6001);
        Check(as && !as->NeedsImport && as->Items.size() == 1 && as->Items.front() == item, "an explicit import durably initializes the bank with exact private extras");
        if (!as) return;
        const auto importedRevision = as->Revision;
        openB.ConfirmImport = true; auto another = item; another.Count = 2; openB.Import.push_back(another);
        Harness::ClearOutbox(); send(bob, openB); send(alice, openA);
        bs = latest.template operator()<Coop::StorageState>(6002); as = latest.template operator()<Coop::StorageState>(6001);
        Check(bs && as && bs->Revision == importedRevision && as->Revision == importedRevision && bs->Items.size() == 1 && bs->Items.front().Count == 1,
            "a repeated import or another character's different old stash cannot merge/overwrite an initialized bank");
        const auto aid = World::ToInteger(*alice->GetCharacter()), bid = World::ToInteger(*bob->GetCharacter());
        const auto se = static_cast<entt::entity>(as->Entity);
        Check(!world.all_of<CellIdComponent>(se), "persistent bank survives without any hidden-cell membership");
        Coop::Transfer take;
        take.Source = as->Entity; take.Destination = aid; take.SourceRevision = as->Revision;
        take.DestinationRevision = world.get<InventoryComponent>(*alice->GetCharacter()).Content.Revision;
        take.DestinationEpoch = world.get<OwnerComponent>(*alice->GetCharacter()).OwnershipEpoch;
        take.Nonce = 601; take.Item = item;
        world.get<ObjectComponent>(accessEntity).CurrentLockData.IsLocked = true;
        Harness::ClearOutbox(); send(alice, take);
        const auto reLocked = latest.template operator()<Coop::TransferReply>(6001);
        Check(reLocked && reLocked->Result == Coop::TransferResult::Rejected,
            "a fresh lease cannot bypass a point locked after opening");
        world.remove<ObjectComponent>(accessEntity);
        Harness::ClearOutbox(); send(alice, take);
        const auto noLockState = latest.template operator()<Coop::TransferReply>(6001);
        Check(noLockState && noLockState->Result == Coop::TransferResult::Rejected,
            "lost point/lock state is not interpreted as an unlocked access");
        world.emplace<ObjectComponent>(accessEntity, alice);
        const auto moveAlice = [&](float x) {
            const auto entity = *alice->GetCharacter();
            ClientReferencesMoveRequest move; move.Tick = world.get<MovementComponent>(entity).Tick + 1;
            auto& update = move.Updates[aid]; update.OwnershipEpoch = take.DestinationEpoch;
            update.CellId = cityA; update.Position = {x, 200.f, 0.f};
            return DeliverWire(world, move, alice);
        };
        Check(moveAlice(10000.f) && world.get<MovementComponent>(*alice->GetCharacter()).Position.x == 10000.f,
            "own-avatar production movement, not a StorageOpen position claim, defines access distance");
        Harness::ClearOutbox(); send(alice, openA); send(alice, Coop::InventoryQuery{as->Entity});
        Check(!latest.template operator()<Coop::StorageState>(6001) && !latest.template operator()<Coop::InventoryState>(6001),
            "far avatar cannot open or read a bank despite a still-fresh valid menu lease");
        Harness::ClearOutbox(); send(alice, take);
        const auto tooFar = latest.template operator()<Coop::TransferReply>(6001);
        Check(tooFar && tooFar->Result == Coop::TransferResult::Rejected && Coop::ExactCount(world.get<InventoryComponent>(se).Content, item) == 1,
            "each transfer rechecks trusted distance; walking away cannot spend an earlier near lease");
        Check(moveAlice(100.f), "own avatar returns near the verified static access");
        send(alice, openA);
        Harness::ClearOutbox(); send(alice, take);
        const auto prepared = latest.template operator()<Coop::TransferReply>(6001);
        Check(prepared && prepared->Result == Coop::TransferResult::Prepared, "nearby access lease authorizes a paired transfer from an off-cell bank");
        if (!prepared || prepared->Result != Coop::TransferResult::Prepared) return;
        auto race = take; race.Destination = bid; race.Nonce = 602;
        race.DestinationRevision = world.get<InventoryComponent>(*bob->GetCharacter()).Content.Revision;
        race.DestinationEpoch = world.get<OwnerComponent>(*bob->GetCharacter()).OwnershipEpoch;
        Harness::ClearOutbox(); send(bob, race);
        const auto losing = latest.template operator()<Coop::TransferReply>(6002);
        Check(losing && losing->Result == Coop::TransferResult::Conflict, "a second city opener cannot reserve the same exact item concurrently");
        Check(Coop::ExactCount(world.get<InventoryComponent>(se).Content, item) == 1, "bank contents stay put before native proof");
        Harness::ClearOutbox(); send(alice, Coop::TransferReceipt{prepared->Operation, true, false});
        const auto committed = latest.template operator()<Coop::TransferReply>(6001);
        const auto remote = latest.template operator()<Coop::InventoryState>(6002);
        Check(committed && committed->Result == Coop::TransferResult::Committed && remote && remote->Entity == as->Entity && remote->Items.empty(),
            "commit removes the bank item once and updates another cohort's authorized open cache");
        bool leakedBag{};
        for (const auto& message : DecodeOutboxMessagesFor<NotifyCoopAction>(6002))
            if (const auto* snapshot = std::get_if<Coop::InventoryState>(&message.Data.Content)) leakedBag |= snapshot->Entity == aid;
        Check(!leakedBag, "a cross-cohort bank subscriber never receives the taker's unrelated personal inventory snapshot");
        Check(Coop::ExactCount(world.get<InventoryComponent>(*alice->GetCharacter()).Content, item) == 1 &&
              Coop::ExactCount(world.get<InventoryComponent>(*bob->GetCharacter()).Content, item) == 0, "exactly one taker owns the imported item");
        Coop::Transfer deposit;
        deposit.Source = aid; deposit.Destination = as->Entity; deposit.Nonce = 603; deposit.Item = item;
        deposit.SourceRevision = world.get<InventoryComponent>(*alice->GetCharacter()).Content.Revision;
        deposit.DestinationRevision = world.get<InventoryComponent>(se).Content.Revision; deposit.SourceEpoch = take.DestinationEpoch;
        Harness::ClearOutbox(); send(alice, deposit);
        const auto dp = latest.template operator()<Coop::TransferReply>(6001);
        Check(dp && dp->Result == Coop::TransferResult::Prepared, "paired deposit reserves own bag and bank, not a unilateral storage AddItem");
        if (!dp || dp->Result != Coop::TransferResult::Prepared) return;
        send(alice, Coop::TransferReceipt{dp->Operation, true, false});
        Check(Coop::ExactCount(world.get<InventoryComponent>(se).Content, item) == 1 && world.get<InventoryComponent>(*alice->GetCharacter()).Content.Entries.empty(),
            "deposit returns the relic to durable storage with no residual personal copy");
        Harness::ClearOutbox(); send(alice, take);
        const auto replay = latest.template operator()<Coop::TransferReply>(6001);
        Check(replay && replay->Result == Coop::TransferResult::Committed && replay->SourceRevision == importedRevision + 1 &&
              replay->SourceRevision < world.get<InventoryComponent>(se).Content.Revision,
            "a historical committed receipt keeps its original post-transfer revision after a later deposit");
        coop.Tick(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) + 3000);
        race.Nonce = 604; race.SourceRevision = world.get<InventoryComponent>(se).Content.Revision;
        Harness::ClearOutbox(); send(bob, race);
        const auto expired = latest.template operator()<Coop::TransferReply>(6002);
        Check(expired && expired->Result == Coop::TransferResult::Rejected, "an expired menu/access lease cannot withdraw even while the bank has the item");
        auto spoofed = openB; spoofed.AccessPoint = GameId(1, 0x999);
        Harness::ClearOutbox(); send(bob, spoofed);
        Check(!latest.template operator()<Coop::StorageState>(6002), "a claimed unlisted access/backing pair is not authorization");
        Disconnect(server, 6001); Disconnect(server, 6002);
    }
    {
        GameServer server(console);
        auto& world = server.GetWorld(); auto& coop = world.ctx().at<CoopService>(); auto& mods = world.ctx().at<ModsComponent>();
        auto* alice = JoinPlayer(world, 6101, "Alice restart", cityA, 611);
        RequestCoopAction hello; hello.Data.Operation = 1;
        hello.Data.Content = Coop::Hello{Coop::kProtocol, Coop::NativeDowned | Coop::TransactionalLoot | Coop::PersistentStorage,
            "bank-alice", "verified-profile", catalog.Signature()};
        Harness::ClearOutbox(); DeliverWire(world, hello, alice);
        Check(!coop.UsesTransactionalInventory(alice) && !mods.Find("RFAB.esp", false), "restart with an absent saved item plugin refuses rather than inventing its dictionary id or replacing the journal");
        Check(mods.AddStandard("RFAB.esp") == 1, "missing plugin can later negotiate normally after the refused attempt");
        alice->SetModIds({0, 1}); hello.Data.Operation = 2;
        DeliverWire(world, hello, alice);
        Check(coop.UsesTransactionalInventory(alice), "matching profile and bag can restore the same journal after an incompatible client was refused");
        AssignObjectsRequest assign; ObjectData point; point.Id = accessA; point.CellId = cityA; assign.Objects.push_back(point); DeliverWire(world, assign, alice);
        RequestCoopAction open; open.Data.Session = coop.Session(); open.Data.Generation = coop.Generation(); open.Data.Operation = 3;
        open.Data.Content = Coop::StorageOpen{"RFAB.esp", 0x800, backing, accessA, false, {}};
        Harness::ClearOutbox(); DeliverWire(world, open, alice);
        const auto state = latest.template operator()<Coop::StorageState>(6101);
        Check(state && !state->NeedsImport && state->Items.size() == 1 && state->Items.front() == item,
            "server restart retains the exact hidden bank and never offers a second initial import");
        Disconnect(server, 6101);
    }
}
