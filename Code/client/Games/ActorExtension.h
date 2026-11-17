#pragma once

#include <Structs/ActionEvent.h>
#include <atomic>
#include <mutex>

struct ActorExtension
{
    enum
    {
        kRemote = 1 << 0,
        kPlayer = 1 << 1,
        kLocalAvatar = 1 << 2,
    };

    enum class ReconciliationStage
    {
        None,
        WaitingForDisable,
        WaitingFor3D
    };

    bool IsRemote() const noexcept;
    bool IsLocal() const noexcept;
    bool IsPlayer() const noexcept;
    bool IsRemotePlayer() const noexcept;
    bool IsLocalPlayer() const noexcept;
    void SetRemote(bool aSet) noexcept;
    void SetPlayer(bool aSet) noexcept;
    void BindLocalAvatar() noexcept;

    struct ProtectionSnapshot
    {
        bool Active{}, Essential{}, NoRecovery{};
    } Protection;
    std::atomic_bool CoopDowned{false}, CoopInputBlocked{false};

    void SetLatestAction(const ActionEvent& aAction) noexcept;
    ActionEvent GetLatestAction() const noexcept;
    size_t GraphDescriptorHash = 0;
    uint64_t GraphDescriptorProjectKey = 0;

    // TODO: atomic? bool instead? maybe simplify to `IsReenabling()` ?
    // Protects discovery while rebuilding a leveled NPC.
    ReconciliationStage Reconciliation{ReconciliationStage::None};

private:
    mutable std::mutex m_actionMutex;
    ActionEvent m_latestAnimation{};
    std::atomic<uint32_t> onlineFlags{0};
};
