#pragma once
#include <Coop/RewardMailbox.h>
#include <Coop/PersonalReward.h>
#include <Coop/RewardProfile.h>
#include <Structs/CoopProtocol.h>
#include <Coop/ClientTimeline.h>
#include <ThreadAffinity.h>
#include <deque>
#include <map>

struct World;
struct TransportService;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct WorldLoadedEvent;
struct CoopActionEvent;
struct BSScript;

// Owned scalar ingress only. Exports/Papyrus native callbacks can run on engine threads.
struct RewardBridgeClient
{
    static Coop::RewardMailbox& Mailbox() noexcept;
    static Coop::PersonalRewardMailbox& PersonalMailbox() noexcept;
    static RFAB_ANY_THREAD bool RequestPersonal(const Coop::PersonalRewardRequest&) noexcept;
    static RFAB_ANY_THREAD bool Register(uint64_t, const Coop::Rewards::Adapter*) noexcept;
    static RFAB_ANY_THREAD uint64_t Epoch() noexcept;
    static RFAB_ANY_THREAD Coop::Rewards::Mode Mode() noexcept;
    static RFAB_ANY_THREAD Coop::Rewards::Submission Submit(const Coop::Rewards::Source*) noexcept;
    static RFAB_ANY_THREAD bool Complete(const Coop::Rewards::Completion*) noexcept;
};

struct RewardService
{
    RewardService(World&, entt::dispatcher&, TransportService&) noexcept;
    ~RewardService() noexcept = default;
    TP_NOCOPYMOVE(RewardService);
    void Describe(Coop::Hello&) const;
    bool ReadyToNegotiate() const;
private:
    struct Input
    {
        Coop::RewardMailbox::Event Event;
        uint64_t Began{}, LastSend{};
        std::optional<Coop::Reward> Packet;
    };
    struct Application
    {
        Coop::RewardGrant Grant;
        Coop::EffectId Effect;
        uint64_t Epoch{}, Token{}, Began{}, LastSend{};
        std::optional<Coop::RewardReceipt> Receipt;
    };
    void OnUpdate(const UpdateEvent&) noexcept;
    void OnConnected(const ConnectedEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnWorldLoaded(const WorldLoadedEvent&) noexcept;
    void OnPacket(const CoopActionEvent&) noexcept;
    void Apply(const Coop::RewardSource&) noexcept;
    void Apply(const Coop::RewardReport&) noexcept;
    void Apply(const Coop::RewardGrant&) noexcept;
    template <class T> void Apply(const T&) noexcept {}
    bool MakePacket(Input&) noexcept;
    void Start(const Coop::RewardGrant&) noexcept;
    void PollApplication() noexcept;
    void ApplyPersonal(const Coop::PersonalRewardRequest&) noexcept;
    void RefreshProfile() noexcept;
    bool ProfileMatches(const Coop::Rewards::Adapter&) const noexcept;
    void Fail(const char*) noexcept;
    uint32_t ServerId(uint32_t aForm) const noexcept;
    uint64_t Now() const noexcept;
    const Coop::SavedRewards* Progress() const noexcept;

    World& m_world;
    TransportService& m_transport;
    uint64_t m_epoch{}, m_tokens{};
    bool m_blocked{}, m_semantic{}, m_tokenSeeded{};
    bool m_profilePresent{}, m_profileValid{};
    std::optional<Coop::RewardProfile> m_profile;
    std::deque<Input> m_inputs;
    std::map<uint32_t, Coop::RewardSource> m_sources;
    std::map<uint64_t, std::pair<Coop::RewardGrant, uint64_t>> m_grants;
    std::map<uint64_t, std::pair<Coop::RewardGrant, Coop::RewardReceipt>> m_completed;
    std::optional<Application> m_application;
    entt::scoped_connection m_update, m_connected, m_disconnected, m_loaded, m_packet;
};
