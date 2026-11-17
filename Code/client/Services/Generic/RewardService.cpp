#include <TiltedOnlinePCH.h>
#include <Services/RewardService.h>
#include <Services/CoopService.h>
#include <Services/SaveStateService.h>
#include <Coop/RewardLedger.h>
#include <Events/UpdateEvent.h>
#include <Events/ConnectedEvent.h>
#include <Events/DisconnectedEvent.h>
#include <Events/WorldLoadedEvent.h>
#include <Events/CoopActionEvent.h>
#include <World.h>
#include <Misc/BSScript.h>
#include <Misc/GameVM.h>
#include <limits>
#include <random>

namespace
{
bool SameGrant(const Coop::RewardGrant& a, const Coop::RewardGrant& b)
{
    return a.RewardId == b.RewardId && a.BaseXP == b.BaseXP && a.BeforeRevision == b.BeforeRevision && a.BeforeDigest == b.BeforeDigest;
}
struct ApplyArguments final : BSScript::IFunctionArguments
{
    std::string Ticket;
    int32_t Base{};
    void Prepare(Statement* args) noexcept override
    {
        args->SetSize(2);
        args->vars[0].Set<const char*>(Ticket.c_str());
        args->vars[1].Set<int32_t>(Base);
    }
};
bool DispatchPapyrus(const Coop::Rewards::Adapter& adapter, uint64_t epoch, uint64_t token, int32_t base,
    const char* eventName = "OnRFABCoopRewardApply")
{
    auto* game = GameVM::Get();
    auto* vm = game ? game->virtualMachine : nullptr;
    auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
    if (!adapter.Receiver || !policy || !policy->IsHandleObjectAvailable(adapter.Receiver)) return false;
    ApplyArguments args;
    args.Ticket = fmt::format("{:016x}:{:016x}", epoch, token); args.Base = base;
    BSFixedString event(eventName);
    vm->SendEvent(adapter.Receiver, event, &args); // the VM packs owned scalar arguments synchronously
    return true; // enqueue is NOT application; the script must complete both pools and return its ticket
}
}
RewardService::RewardService(World& world, entt::dispatcher& dispatcher, TransportService& transport) noexcept
    : m_world(world), m_transport(transport)
{
    try
    {
        std::random_device random;
        // ESS may retain a delayed VM stack across a process restart, whose load epoch starts
        // counting again. Never restart application tickets at the predictable value one.
        m_tokens = ((uint64_t(random()) << 32) ^ random()) & ((1ull << 60) - 1);
        m_tokenSeeded = true;
    }
    catch (const std::exception& error)
    {
        m_blocked = true;
        spdlog::error("[RFAB] cannot seed native XP tickets; semantic XP disabled: {}", error.what());
    }
    m_update = dispatcher.sink<UpdateEvent>().connect<&RewardService::OnUpdate>(this);
    m_connected = dispatcher.sink<ConnectedEvent>().connect<&RewardService::OnConnected>(this);
    m_disconnected = dispatcher.sink<DisconnectedEvent>().connect<&RewardService::OnDisconnected>(this);
    m_loaded = dispatcher.sink<WorldLoadedEvent>().connect<&RewardService::OnWorldLoaded>(this);
    m_packet = dispatcher.sink<CoopActionEvent>().connect<&RewardService::OnPacket>(this);
}
uint64_t RewardService::Now() const noexcept
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
uint32_t RewardService::ServerId(uint32_t form) const noexcept
{
    for (auto entity : m_world.view<FormIdComponent>())
        if (m_world.get<FormIdComponent>(entity).Id == form) return Utils::GetServerId(entity).value_or(0);
    return 0;
}
const Coop::SavedRewards* RewardService::Progress() const noexcept
{
    const auto& campaign = m_world.ctx().at<CoopService>().Campaign();
    for (const auto& progress : m_world.ctx().at<SaveStateService>().State().Rewards)
        if (progress.Campaign == campaign) return &progress;
    return nullptr;
}
void RewardService::Describe(Coop::Hello& hello) const
{
    const auto& saves = m_world.ctx().at<SaveStateService>();
    for (const auto& progress : saves.State().Rewards) hello.Rewards.push_back({progress.Campaign, progress.Revision, progress.Digest});
    const auto adapter = RewardBridgeClient::Mailbox().Adapter(SaveStateBridge::Epoch());
    if (m_blocked || !saves.Ready() || !adapter || !ProfileMatches(*adapter)) return;
    hello.Capabilities |= Coop::SemanticRewards | ((adapter->Flags & Coop::Rewards::Bizarre) ? Coop::BizarreAdapter : 0u);
    hello.RewardSignature = adapter->Signature; hello.RewardKinds = adapter->Kinds;
}
void RewardService::OnConnected(const ConnectedEvent&) noexcept
{
    RewardBridgeClient::Mailbox().Configure(SaveStateBridge::Epoch(), Coop::Rewards::Mode::Negotiating);
}
void RewardService::OnDisconnected(const DisconnectedEvent& event) noexcept
{
    const bool uncertain = m_application.has_value() || !m_inputs.empty() || !m_grants.empty() || RewardBridgeClient::Mailbox().HasPending();
    if (m_application) m_world.ctx().at<SaveStateService>().Uncertain(m_application->Effect);
    m_inputs.clear(); m_grants.clear(); m_sources.clear(); m_completed.clear(); m_application.reset(); m_semantic = false;
    if (event.ForWorldLoad) return; // new epoch already invalidated the old mailbox; never block a new save's registration here
    if (uncertain)
    {
        m_blocked = true; RewardBridgeClient::Mailbox().Block(); m_world.ctx().at<SaveStateService>().Quarantine();
        Utils::ShowHudMessage("RFAB: disconnect with an unresolved semantic reward. Shared XP locked; no local fallback or reconnect catch-up.");
    }
    else RewardBridgeClient::Mailbox().Configure(SaveStateBridge::Epoch(), Coop::Rewards::Mode::Offline);
}
void RewardService::OnWorldLoaded(const WorldLoadedEvent&) noexcept
{
    m_epoch = SaveStateBridge::Epoch();
    RewardBridgeClient::Mailbox().NewEpoch(m_epoch);
    RewardBridgeClient::PersonalMailbox().NewEpoch(m_epoch);
    m_inputs.clear(); m_grants.clear(); m_sources.clear(); m_completed.clear(); m_application.reset();
    m_blocked = !m_tokenSeeded; m_semantic = false;
    RefreshProfile();
}
void RewardService::Fail(const char* reason) noexcept
{
    if (m_blocked) return;
    const std::string owned = reason ? reason : "Unknown semantic XP failure";
    m_blocked = true;
    RewardBridgeClient::Mailbox().Block();
    auto& saves = m_world.ctx().at<SaveStateService>();
    if (m_application) saves.Uncertain(m_application->Effect);
    saves.Quarantine();
    spdlog::error("[RFAB] semantic XP quarantined: {}", owned);
    Utils::ShowHudMessage("RFAB: semantic XP/receipt mismatch. Shared effects locked; load a matching save. No wallet normalization or automatic repeat.");
    m_transport.Close();
}
bool RewardService::MakePacket(Input& input) noexcept
{
    if (input.Packet) return true;
    const auto& source = input.Event.Source;
    Coop::Reward reward;
    reward.Kind = static_cast<Coop::RewardKind>(source.Kind); reward.BaseXP = source.BaseXP; reward.Nonce = input.Event.Nonce;
    reward.Incarnation = source.Incarnation; reward.Occurrence = source.Occurrence;
    auto& mods = m_world.GetModSystem();
    if (!mods.GetServerModId(source.SourceForm, reward.Source) || reward.Source.ModId >= 0xFFFFFFFEu ||
        (source.ContextForm && (!mods.GetServerModId(source.ContextForm, reward.Context) || reward.Context.ModId >= 0xFFFFFFFEu)))
    { Fail("Semantic reward source/context cannot resolve to a stable installed form"); return false; }
    if (reward.Kind == Coop::RewardKind::Kill)
    {
        reward.Actor = ServerId(source.ActorForm);
        const auto context = m_sources.find(reward.Actor);
        if (!reward.Actor || context == m_sources.end()) return false; // death packet and delayed Papyrus callback may cross
        if (source.SourceForm != source.ActorForm || context->second.Source != reward.Source ||
            (source.Incarnation && source.Incarnation != context->second.Incarnation))
        { Fail("Native kill event does not match its authoritative reference/life token"); return false; }
        reward.Incarnation = context->second.Incarnation; reward.Occurrence = 1;
    }
    else
    {
        if (source.ActorForm && source.ActorForm != 0x14) { Fail("Semantic event attempts to issue another avatar's reward"); return false; }
        reward.Actor = ServerId(0x14); if (!reward.Actor) return false;
    }
    input.Packet = reward; return true;
}
void RewardService::OnUpdate(const UpdateEvent&) noexcept
{
    if (!RFAB_EXPECT_MAIN_THREAD() || m_world.IsLoadingWorld()) return;
    auto& bridge = RewardBridgeClient::Mailbox();
    if (bridge.ConsumeFault()) { Fail("Native semantic adapter/queue/completion violated its bounded generation contract"); return; }
    if (m_blocked) return;
    auto& coop = m_world.ctx().at<CoopService>();
    m_semantic = coop.Supports(Coop::SemanticRewards);
    const auto adapter = bridge.Adapter(m_epoch);
    if (coop.Active() && adapter && (!m_semantic || !ProfileMatches(*adapter)))
    { Fail("Installed XP adapter appeared after negotiation or no longer matches its compiled profile; reconnect after a clean load"); return; }
    if (!m_semantic) return;
    if (m_epoch != SaveStateBridge::Epoch() || !adapter || !ProfileMatches(*adapter) || !m_world.ctx().at<SaveStateService>().Ready())
    { Fail("Semantic XP no longer owns a matching loaded native timeline"); return; }
    bridge.Configure(m_epoch, Coop::Rewards::Mode::Active);
    for (auto& event : bridge.Take(m_epoch))
    {
        if (m_inputs.size() >= Coop::RewardMailbox::kCapacity) { Fail("Semantic reward inbox overflow"); return; }
        m_inputs.push_back({std::move(event), Now(), 0, {}});
    }
    for (auto& input : m_inputs)
    {
        if (Now() - input.Began >= 5000) { Fail("Semantic source/acknowledgement unavailable within five seconds"); return; }
        if (!MakePacket(input)) { if (m_blocked) return; continue; }
        if (!input.LastSend || Now() - input.LastSend >= 500)
        { input.LastSend = Now(); coop.SendAction(*input.Packet); }
    }
    PollApplication();
    if (m_blocked || m_application || m_grants.empty()) return;
    if (Now() - m_grants.begin()->second.second >= 5000) { Fail("Semantic grant could not acquire its native save boundary"); return; }
    const auto grant = m_grants.begin()->second.first;
    Start(grant);
}
void RewardService::OnPacket(const CoopActionEvent& event) noexcept
{
    if (!RFAB_EXPECT_MAIN_THREAD()) return;
    std::visit([this](const auto& body) { Apply(body); }, event.Data.Content);
}
void RewardService::Apply(const Coop::RewardSource& source) noexcept
{
    if (!m_world.ctx().at<CoopService>().Supports(Coop::SemanticRewards) || m_blocked) return;
    if (m_sources.size() >= 8192 && !m_sources.contains(source.Actor)) { Fail("Kill-life inbox limit reached"); return; }
    const auto old = m_sources.find(source.Actor);
    if (old != m_sources.end() && (old->second.Incarnation != source.Incarnation || old->second.Source != source.Source))
    { Fail("Kill-life source changed without a coordinated lifecycle transition"); return; }
    m_sources[source.Actor] = source;
}
void RewardService::Apply(const Coop::RewardReport& report) noexcept
{
    const auto it = std::find_if(m_inputs.begin(), m_inputs.end(), [&](const auto& input) { return input.Event.Nonce == report.Nonce; });
    if (it == m_inputs.end()) return;
    if (!report.Accepted) { Fail(report.Reason.c_str()); return; }
    m_inputs.erase(it);
}
void RewardService::Apply(const Coop::RewardGrant& grant) noexcept
{
    auto& coop = m_world.ctx().at<CoopService>();
    if (!coop.Supports(Coop::SemanticRewards) || m_blocked) return;
    const auto completed = m_completed.find(grant.RewardId);
    if (completed != m_completed.end())
    {
        if (!SameGrant(grant, completed->second.first)) { Fail("A repeated reward grant changed its frozen base/receipt chain"); return; }
        if (grant.Result == Coop::RewardResult::Offered) coop.SendAction(completed->second.second);
        return; // a committed confirmation NEVER calls the adapter again
    }
    if (m_application && m_application->Grant.RewardId == grant.RewardId)
    {
        if (!SameGrant(grant, m_application->Grant)) { Fail("Reward grant identity changed during native execution"); return; }
        if (grant.Result == Coop::RewardResult::Offered)
        { if (m_application->Receipt) coop.SendAction(*m_application->Receipt); return; }
        if (grant.Result == Coop::RewardResult::Committed)
        {
            const auto application = *m_application;
            if (!application.Receipt || !application.Receipt->Applied || application.Receipt->Uncertain ||
                !m_world.ctx().at<SaveStateService>().Commit(application.Effect) ||
                !RewardBridgeClient::Mailbox().FinishApplication(application.Epoch, application.Token))
            { Fail("Server reward commit has no matching durable dual-pool native receipt"); return; }
            m_completed.emplace(grant.RewardId, std::make_pair(application.Grant, *application.Receipt));
            m_application.reset();
            spdlog::info("[RFAB] semantic reward {} committed, base {}, personal final {}", grant.RewardId, grant.BaseXP, application.Receipt->FinalXP);
            const auto text = fmt::format("RFAB: +{} XP (shared base {}, personal multiplier)", application.Receipt->FinalXP, grant.BaseXP);
            Utils::ShowHudMessage(String(text.c_str()));
            if (const auto adapter = RewardBridgeClient::Mailbox().Adapter(application.Epoch); adapter && adapter->Receiver)
                DispatchPapyrus(*adapter, application.Epoch, application.Token, application.Receipt->FinalXP, "OnRFABCoopRewardApplied");
            return;
        }
        Fail(grant.Reason.empty() ? "Semantic reward rejected/quarantined after native admission" : grant.Reason.c_str()); return;
    }
    const auto* progress = Progress();
    if (grant.Result == Coop::RewardResult::Committed && progress && progress->Revision > grant.BeforeRevision)
    {
        if (progress->Revision == grant.BeforeRevision + 1 && (progress->LastReward != grant.RewardId || progress->BaseXP != grant.BaseXP ||
            progress->Digest != Coop::AdvanceRewardDigest(grant.BeforeDigest, grant.RewardId, grant.BaseXP)))
            Fail("Committed semantic reward disagrees with the loaded co-save receipt");
        return; // older committed event is confirmation only, even after personal spending
    }
    if (grant.Result != Coop::RewardResult::Offered) { Fail("Unresolved semantic reward has no native receipt in this timeline"); return; }
    const auto old = m_grants.find(grant.RewardId);
    if (old != m_grants.end()) { if (!SameGrant(old->second.first, grant)) Fail("Queued semantic grant changed"); return; }
    if (m_grants.size() >= 64) { Fail("Semantic grant inbox overflow"); return; }
    m_grants.emplace(grant.RewardId, std::make_pair(grant, Now()));
}
void RewardService::Start(const Coop::RewardGrant& grant) noexcept
{
    auto& coop = m_world.ctx().at<CoopService>();
    auto& saves = m_world.ctx().at<SaveStateService>();
    auto& bridge = RewardBridgeClient::Mailbox();
    const auto adapter = bridge.Adapter(m_epoch);
    const auto* progress = Progress();
    if (!adapter || !ProfileMatches(*adapter) || (progress ? progress->Revision : 0) != grant.BeforeRevision || (progress ? progress->Digest : 0) != grant.BeforeDigest)
    { Fail("Native reward adapter or before-receipt chain differs from the server offer"); return; }
    const auto effect = Coop::NativeEffect(Coop::EffectKind::Reward, coop.Session(), coop.Generation(), grant.RewardId);
    if (!saves.Begin(Coop::EffectKind::Reward, effect)) return; // defer a pinned save or another serial effect; never block the game thread
    if (m_tokens == std::numeric_limits<uint64_t>::max() || !bridge.BeginApplication(m_epoch, ++m_tokens))
    { saves.AbortBeforeNative(effect); Fail("Cannot reserve a generation-bound native reward token"); return; }
    m_application = Application{grant, effect, m_epoch, m_tokens, Now(), 0, {}};
    m_grants.erase(grant.RewardId);
    const auto application = *m_application;
    try
    {
        if (adapter->ApplyReward)
        {
            const auto result = adapter->ApplyReward(application.Epoch, application.Token, grant.BaseXP);
            if (application.Epoch != SaveStateBridge::Epoch() || m_world.IsLoadingWorld()) return;
            if (!m_application || m_application->Token != application.Token) { saves.Uncertain(effect); return; }
            if (result.State != Coop::Rewards::Result::Pending)
            { if (!bridge.Complete(application.Epoch, result)) Fail("RFAB dual-pool apply callback returned an invalid receipt"); }
            else if (result.WorldEpoch != application.Epoch || result.Token != application.Token || result.FinalXP || result.Pools)
                Fail("RFAB pending apply callback returned a different epoch/token or an ambiguous partial result");
        }
        else if (!DispatchPapyrus(*adapter, application.Epoch, application.Token, grant.BaseXP))
        {
            saves.AbortBeforeNative(effect);
            coop.SendAction(Coop::RewardReceipt{grant.RewardId, false, false, 0, grant.BeforeRevision, grant.BeforeDigest});
            Fail("Papyrus reward receiver disappeared before native application");
        }
    }
    catch (...) { Fail("Exception in the installed RFAB reward adapter; no native replay"); }
}
void RewardService::PollApplication() noexcept
{
    if (!m_application) return;
    auto& coop = m_world.ctx().at<CoopService>();
    auto& saves = m_world.ctx().at<SaveStateService>();
    const auto application = *m_application;
    if (application.Epoch != SaveStateBridge::Epoch()) return;
    if (!application.Receipt)
    {
        if (const auto request = RewardBridgeClient::PersonalMailbox().Take(application.Epoch, application.Token)) ApplyPersonal(*request);
        if (m_blocked || !m_application || application.Epoch != SaveStateBridge::Epoch() || m_world.IsLoadingWorld()) return;
        const auto completion = RewardBridgeClient::Mailbox().Completion(application.Epoch, application.Token);
        if (completion)
        {
            const auto& grant = application.Grant;
            if (completion->State != Coop::Rewards::Result::Applied)
            {
                const bool uncertain = completion->State != Coop::Rewards::Result::NotApplied;
                if (!uncertain) saves.AbortBeforeNative(application.Effect); else saves.Uncertain(application.Effect);
                coop.SendAction(Coop::RewardReceipt{grant.RewardId, false, uncertain, 0, grant.BeforeRevision, grant.BeforeDigest});
                Fail("RFAB did not verify application to both personal XP pools"); return;
            }
            const auto revision = grant.BeforeRevision + 1;
            const auto digest = Coop::AdvanceRewardDigest(grant.BeforeDigest, grant.RewardId, grant.BaseXP);
            if (!saves.Applied(application.Effect) || !saves.StampReward({coop.Campaign(), revision, digest, grant.RewardId, grant.BaseXP, completion->FinalXP}))
            {
                saves.Uncertain(application.Effect);
                coop.SendAction(Coop::RewardReceipt{grant.RewardId, false, true, 0, grant.BeforeRevision, grant.BeforeDigest});
                Fail("Dual-pool reward applied but its co-save/write-ahead receipt could not be published"); return;
            }
            m_application->Receipt = Coop::RewardReceipt{grant.RewardId, true, false, completion->FinalXP, revision, digest};
            m_application->Began = Now();
        }
    }
    if (m_application && m_application->Receipt && (!m_application->LastSend || Now() - m_application->LastSend >= 500))
    { m_application->LastSend = Now(); coop.SendAction(*m_application->Receipt); }
    if (m_application && Now() - m_application->Began >= 5000)
    {
        if (!m_application->Receipt)
            coop.SendAction(Coop::RewardReceipt{application.Grant.RewardId, false, true, 0, application.Grant.BeforeRevision, application.Grant.BeforeDigest});
        Fail("RFAB native reward completion/server commit timed out; late callback cannot execute it a second time");
    }
}
