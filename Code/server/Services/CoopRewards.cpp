#include <Services/CoopService.h>
#include <World.h>
#include <Components.h>
#include <Game/Player.h>
#include <algorithm>
#include <limits>

void CoopService::InitializeRewards() noexcept
{
    // One campaign lease protects both journals. Never bootstrap another writer or replace a
    // corrupt XP ledger with an empty wallet. The two files are NOT a coordinated checkpoint.
    if (!m_inventoryLease || !m_inventoryLease->Acquired() || m_campaign.empty()) return;
    m_rewardFile = std::make_unique<Coop::DurableFile>(std::filesystem::path("Data/rfab_campaign") / (m_campaign + ".rewards"));
    m_rewards = std::make_unique<Coop::RewardLedger>(m_campaign, [this](const auto& state) {
        const auto bytes = Coop::RewardLedger::Encode(state);
        return bytes && m_rewardFile->Write(*bytes);
    });
    std::vector<uint8_t> bytes;
    const auto read = m_rewardFile->Read(bytes);
    if (read == Coop::DurableFile::ReadResult::Failed) { m_rewards.reset(); return; }
    if (read == Coop::DurableFile::ReadResult::Loaded)
    {
        auto state = Coop::RewardLedger::Decode(bytes);
        if (!state || !m_rewards->Restore(std::move(*state)))
        {
            spdlog::error("[RFAB] semantic reward journal is invalid/unwritable. No empty bootstrap or XP catch-up");
            m_rewards.reset();
        }
    }
    m_rewardNonce = m_session;
}
bool CoopService::RewardsReady() const noexcept { return m_rewards && !m_rewards->Faulted(); }
bool CoopService::UsesSemanticRewards(Player* player) const noexcept
{
    if (!player) return false;
    const auto it = m_clients.find(player->GetId());
    return it != m_clients.end() && (it->second.Capabilities & Coop::SemanticRewards);
}
bool CoopService::AttachRewards(const Coop::Hello& hello, uint32_t group) noexcept
{
    const bool semantic = (hello.Capabilities & Coop::SemanticRewards) != 0;
    for (const auto& [_, member] : m_clients)
        if (member.Group == group && (bool(member.Capabilities & Coop::SemanticRewards) != semantic ||
            (semantic && (member.RewardSignature != hello.RewardSignature || member.RewardKinds != hello.RewardKinds ||
                bool(member.Capabilities & Coop::BizarreAdapter) != bool(hello.Capabilities & Coop::BizarreAdapter))))) return false;
    if (!semantic)
    {
        // Removing/losing the adapter cannot silently resume legacy local awards for an account
        // already bound to semantic receipts. New unrelated non-RFAB clients retain STR behavior.
        const auto* account = m_rewards ? m_rewards->FindAccount(hello.Character) : nullptr;
        return !account || (!account->Revision && !account->Blocked);
    }
    if (!RewardsReady() || !hello.RewardSignature || !hello.RewardKinds || (hello.RewardKinds & ~15u)) return false;
    uint64_t revision{}, digest{};
    for (const auto& progress : hello.Rewards)
        if (progress.Campaign == m_campaign) { revision = progress.Revision; digest = progress.Digest; }
    return m_rewards->Attach(hello.Character, revision, digest);
}
std::vector<std::string> CoopService::RewardRecipients(uint32_t group, const Coop::Pose& origin, double radius) const
{
    std::vector<std::string> recipients;
    const auto runtime = m_groups.find(group);
    if (runtime == m_groups.end() || !origin.Valid()) return recipients;
    for (const auto& [_, client] : m_clients)
    {
        if (client.Group != group || !(client.Capabilities & Coop::SemanticRewards)) continue;
        const auto member = runtime->second.Encounter.Members.find(client.Actor);
        if (member == runtime->second.Encounter.Members.end() || !member->second.Connected || !member->second.Seen ||
            Now() < member->second.Seen || Now() - member->second.Seen > Coop::kHeartbeatTimeoutMs ||
            (member->second.Flags & Coop::Loading)) continue;
        const auto entity = static_cast<entt::entity>(client.Actor);
        const auto* movement = m_world.try_get<MovementComponent>(entity);
        const auto* cell = m_world.try_get<CellIdComponent>(entity);
        const auto* owner = m_world.try_get<OwnerComponent>(entity);
        if (!movement || !cell || !owner || !owner->GetOwner() || owner->GetOwner()->GetId() != client.PlayerId) continue;
        Coop::Pose position;
        position.Cell = cell->Cell; position.World = cell->WorldSpaceId;
        position.X = movement->Position.x; position.Y = movement->Position.y; position.Z = movement->Position.z;
        if (Coop::DistanceSquared(position, origin) <= radius * radius) recipients.push_back(client.Character);
        // Downed is deliberately NOT excluded. Unrelated/loading/stale/far-away actors are.
    }
    return recipients;
}
void CoopService::RecordKill(Player* player, uint32_t actor, uint32_t epoch) noexcept
{
    if (!RewardsReady() || !UsesSemanticRewards(player)) return;
    const auto client = m_clients.find(player->GetId());
    const auto entity = static_cast<entt::entity>(actor);
    const auto* character = m_world.try_get<CharacterComponent>(entity);
    const auto* owner = m_world.try_get<OwnerComponent>(entity);
    const auto* form = m_world.try_get<FormIdComponent>(entity);
    const auto* cell = m_world.try_get<CellIdComponent>(entity);
    const auto* movement = m_world.try_get<MovementComponent>(entity);
    if (client == m_clients.end() || !character || character->IsDead() || character->IsPlayer() || character->IsPlayerSummon() ||
        character->IsMount() || !owner || !owner->IsCurrentOwner(player, epoch) || !form || !NameForm(form->Id) ||
        !cell || !movement || m_killSources.contains(actor) || m_killSources.size() >= 8192) return;
    Coop::Pose position;
    position.Cell = cell->Cell; position.World = cell->WorldSpaceId;
    position.X = movement->Position.x; position.Y = movement->Position.y; position.Z = movement->Position.z;
    auto recipients = RewardRecipients(client->second.Group, position, 4096.0);
    if (recipients.empty()) return;
    uint64_t life{};
    if (const auto* av = m_world.try_get<ActorValuesComponent>(entity)) life = av->CurrentActorValues.Scale.Life;
    if (!life) { if (++m_rewardNonce == 0) ++m_rewardNonce; life = m_rewardNonce; }
    KillSource source{{actor, epoch, life, form->Id}, client->second.Group, true, std::move(recipients)};
    m_killSources.emplace(actor, source);
    for (const auto& [id, member] : m_clients)
        if (std::find(source.Recipients.begin(), source.Recipients.end(), member.Character) != source.Recipients.end())
            Send(m_world.GetPlayerManager().GetById(id), source.Wire);
    spdlog::info("[RFAB] semantic kill source actor {:X}, life {}, group {}, recipients {}", actor, life, source.Group, source.Recipients.size());
}
Coop::RewardGrant CoopService::RewardGrant(const Coop::RewardLedger::Record& record, const std::string& character) const
{
    const auto& delivery = record.Recipients.at(character);
    const auto state = delivery.State;
    const auto result = state == Coop::RewardLedger::Stage::Offered ? Coop::RewardResult::Offered :
        state == Coop::RewardLedger::Stage::Committed ? Coop::RewardResult::Committed :
        state == Coop::RewardLedger::Stage::Rejected ? Coop::RewardResult::Rejected : Coop::RewardResult::Quarantined;
    return {record.Id, record.BaseXP, result, delivery.BeforeRevision, delivery.BeforeDigest,
        result == Coop::RewardResult::Quarantined ? "Native award receipt is uncertain. No automatic XP replay/catch-up" : ""};
}
void CoopService::Handle(Player* player, Client& client, const Coop::Reward& reward) noexcept
{
    const auto reject = [&](const char* reason) { Send(player, Coop::RewardReport{reward.Nonce, 0, false, reason}); };
    if (!(client.Capabilities & Coop::SemanticRewards) || !(client.RewardKinds & (1u << static_cast<uint8_t>(reward.Kind))) || !RewardsReady())
    { reject("Semantic XP adapter or durable campaign journal is unavailable"); return; }
    const auto source = NameForm(reward.Source);
    const auto context = reward.Context ? NameForm(reward.Context) : std::optional<Coop::StableForm>{};
    if (!source || (reward.Context && !context) || !player->HasMod(reward.Source.ModId) ||
        (reward.Context && !player->HasMod(reward.Context.ModId))) { reject("Reward source/context is not in the negotiated installed profile"); return; }
    Coop::RewardIdentity identity{reward.Kind, *source, context, reward.Incarnation, reward.Occurrence};
    if (!Coop::SemanticRewardKey(identity)) { reject("Invalid semantic reward identity"); return; }
    std::vector<std::string> recipients;
    if (reward.Kind == Coop::RewardKind::Kill)
    {
        const auto kill = m_killSources.find(reward.Actor);
        if (kill == m_killSources.end() || !kill->second.Dead || kill->second.Group != client.Group ||
            kill->second.Wire.Incarnation != reward.Incarnation || kill->second.Wire.Source != reward.Source ||
            reward.Occurrence != 1 || reward.Context ||
            std::find(kill->second.Recipients.begin(), kill->second.Recipients.end(), client.Character) == kill->second.Recipients.end())
        { reject("Kill lacks an authoritative reference-life/death token for this participant"); return; }
        recipients = kill->second.Recipients;
    }
    else
    {
        if (reward.Actor != client.Actor) { reject("Only an owned avatar may issue this semantic reward"); return; }
        const auto entity = static_cast<entt::entity>(client.Actor);
        const auto* movement = m_world.try_get<MovementComponent>(entity);
        const auto* cell = m_world.try_get<CellIdComponent>(entity);
        if (!movement || !cell) { reject("Reward issuer has no accepted avatar position"); return; }
        Coop::Pose origin;
        origin.Cell = cell->Cell; origin.World = cell->WorldSpaceId;
        origin.X = movement->Position.x; origin.Y = movement->Position.y; origin.Z = movement->Position.z;
        recipients = RewardRecipients(client.Group, origin, reward.Kind == Coop::RewardKind::Quest ? 8192.0 :
            reward.Kind == Coop::RewardKind::Book ? 2048.0 : 4096.0);
        if (std::find(recipients.begin(), recipients.end(), client.Character) == recipients.end())
        { reject("Reward issuer is stale/loading/outside the admitted cohort"); return; }
    }
    const auto id = m_rewards->Create(std::move(identity), reward.BaseXP, recipients);
    if (!id) { reject("Reward ledger cannot reserve this event/recipient set; no local fallback"); return; }
    Send(player, Coop::RewardReport{reward.Nonce, *id, true, ""});
    // An old semantic event may be reported again after spending/reconnect. A committed reply
    // is confirmation only; it never grants again or extends the original participant set.
    const auto* record = m_rewards->Find(*id);
    if (const auto it = record->Recipients.find(client.Character); it != record->Recipients.end() &&
        it->second.State == Coop::RewardLedger::Stage::Committed) Send(player, RewardGrant(*record, client.Character));
    TickRewards();
}
void CoopService::Handle(Player* player, Client& client, const Coop::RewardReceipt& receipt) noexcept
{
    if (!(client.Capabilities & Coop::SemanticRewards) || !RewardsReady()) return;
    if (!m_rewards->Receipt(receipt.RewardId, client.Character, receipt.Applied, receipt.Uncertain,
        receipt.FinalXP, receipt.Revision, receipt.Digest)) return;
    const auto* record = m_rewards->Find(receipt.RewardId);
    if (record) Send(player, RewardGrant(*record, client.Character));
    TickRewards();
}
void CoopService::TickRewards() noexcept
{
    if (!RewardsReady()) return;
    if (!m_rewards->Expire(Now())) return;
    const bool resend = !m_rewardResend || Now() - m_rewardResend >= 500;
    if (resend) m_rewardResend = Now();
    for (const auto& [id, client] : m_clients)
    {
        if (!(client.Capabilities & Coop::SemanticRewards)) continue;
        auto* player = m_world.GetPlayerManager().GetById(id);
        if (!player) continue;
        if (const auto record = m_rewards->OfferNext(client.Character, Now())) Send(player, RewardGrant(*record, client.Character));
        else if (resend)
            for (const auto& [_, record] : m_rewards->Snapshot().Records)
                if (const auto it = record.Recipients.find(client.Character); it != record.Recipients.end() &&
                    (it->second.State == Coop::RewardLedger::Stage::Offered || it->second.State == Coop::RewardLedger::Stage::Uncertain))
                { Send(player, RewardGrant(record, client.Character)); break; }
    }
}
