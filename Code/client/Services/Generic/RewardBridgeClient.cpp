#include <TiltedOnlinePCH.h>
#include <Services/RewardService.h>
#include <Services/SaveStateService.h>
#include <World.h>
#include <windows.h>

Coop::RewardMailbox& RewardBridgeClient::Mailbox() noexcept
{
    static Coop::RewardMailbox mailbox;
    return mailbox;
}
Coop::PersonalRewardMailbox& RewardBridgeClient::PersonalMailbox() noexcept
{
    static Coop::PersonalRewardMailbox mailbox;
    return mailbox;
}
bool RewardBridgeClient::RequestPersonal(const Coop::PersonalRewardRequest& request) noexcept
{
    if (request.Epoch != SaveStateBridge::Epoch() || !Mailbox().ClaimApplication(request.Epoch, request.Token)) return false;
    PersonalMailbox().NewEpoch(request.Epoch);
    if (PersonalMailbox().Submit(request)) return true;
    Mailbox().Complete(request.Epoch, {request.Epoch, request.Token, Coop::Rewards::Result::NotApplied, 0, 0});
    return false;
}
bool RewardBridgeClient::Register(uint64_t epoch, const Coop::Rewards::Adapter* adapter) noexcept
{
    if (!adapter || epoch != SaveStateBridge::Epoch()) return false;
    if (adapter->ApplyReward)
    {
        HMODULE module{};
        // A registered callback must live in a loaded module that remains loaded. Never keep a
        // context/actor pointer or invoke an unloaded RFAB.dll after a world transition.
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(adapter->ApplyReward), &module)) return false;
    }
    const bool registered = Mailbox().Register(epoch, *adapter);
    if (registered) PersonalMailbox().NewEpoch(epoch);
    return registered;
}
uint64_t RewardBridgeClient::Epoch() noexcept { return SaveStateBridge::Epoch(); }
Coop::Rewards::Mode RewardBridgeClient::Mode() noexcept { return Mailbox().Mode(); }
Coop::Rewards::Submission RewardBridgeClient::Submit(const Coop::Rewards::Source* source) noexcept
{
    if (!source || source->WorldEpoch != SaveStateBridge::Epoch()) return Coop::Rewards::Submission::Blocked;
    auto& mailbox = Mailbox();
    // Cached atomic network state only: no World::Get/ECS/transport work in a native callback.
    if (World::IsOnlineCached() && mailbox.Mode() == Coop::Rewards::Mode::Offline)
        mailbox.Configure(source->WorldEpoch, Coop::Rewards::Mode::Negotiating);
    return mailbox.Submit(source->WorldEpoch, *source);
}
bool RewardBridgeClient::Complete(const Coop::Rewards::Completion* completion) noexcept
{
    return completion && completion->WorldEpoch == SaveStateBridge::Epoch() && Mailbox().Complete(completion->WorldEpoch, *completion);
}
extern "C" __declspec(dllexport) const Coop::Rewards::Host* RFAB_GetRewardBridge(uint32_t version)
{
    static const Coop::Rewards::Host host{Coop::Rewards::kVersion, sizeof(Coop::Rewards::Host), RewardBridgeClient::Register,
        RewardBridgeClient::Epoch, RewardBridgeClient::Mode, RewardBridgeClient::Submit, RewardBridgeClient::Complete};
    return version == Coop::Rewards::kVersion ? &host : nullptr;
}
