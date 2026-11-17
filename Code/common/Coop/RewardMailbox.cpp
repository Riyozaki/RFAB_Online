#include <Coop/RewardMailbox.h>
#include <Coop/RewardLedger.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <string_view>

namespace Coop
{
namespace
{
bool ValidAdapter(const Rewards::Adapter& a)
{
    const auto end = std::find(std::begin(a.Name), std::end(a.Name), '\0');
    if (end == std::end(a.Name) || end == std::begin(a.Name) || a.Version != Rewards::kVersion || a.Size != sizeof(a) ||
        !a.Signature || !a.Kinds || (a.Kinds & ~15u) || (a.Flags & Rewards::kRequiredFlags) != Rewards::kRequiredFlags ||
        (a.Flags & ~(Rewards::kRequiredFlags | Rewards::Bizarre)) || bool(a.Receiver) == bool(a.ApplyReward)) return false;
    for (auto it = std::begin(a.Name); it != end; ++it)
        if (!( (*it >= 'a' && *it <= 'z') || (*it >= 'A' && *it <= 'Z') || (*it >= '0' && *it <= '9') ||
            *it == '-' || *it == '_' || *it == '.')) return false;
    return true;
}
bool SameAdapter(const Rewards::Adapter& a, const Rewards::Adapter& b)
{
    return a.Signature == b.Signature && a.Kinds == b.Kinds && a.Flags == b.Flags && a.Receiver == b.Receiver &&
        a.ApplyReward == b.ApplyReward && std::string_view(a.Name) == std::string_view(b.Name);
}
bool ValidSource(const Rewards::Source& s)
{
    if (s.Size != sizeof(s) || s.Kind > 3 || !s.SourceForm || !s.Occurrence || s.BaseXP <= 0 || s.BaseXP > RewardLedger::kMaxBaseXP) return false;
    if (s.Kind == static_cast<uint32_t>(RewardKind::Kill) && (!s.ActorForm || s.ContextForm || s.Occurrence != 1)) return false;
    if (s.Kind == static_cast<uint32_t>(RewardKind::Book) && (s.ContextForm || s.Incarnation || s.Occurrence != 1)) return false;
    return true;
}
}
void RewardMailbox::Reset(uint64_t epoch)
{
    m_epoch = epoch; m_token = m_sequence = 0; m_mode = Rewards::Mode::Offline;
    m_adapter.reset(); m_completion.reset(); m_events.clear(); m_fault = m_broken = m_claimed = false;
}
void RewardMailbox::Fault() { m_mode = Rewards::Mode::Blocked; m_fault = m_broken = true; }
void RewardMailbox::NewEpoch(uint64_t epoch)
{
    std::lock_guard lock(m_mutex);
    if (epoch > m_epoch) Reset(epoch); // retain an adapter registered by OnPlayerLoadGame in this SAME epoch
}
bool RewardMailbox::Register(uint64_t epoch, const Rewards::Adapter& adapter)
{
    std::lock_guard lock(m_mutex);
    if (!epoch || !ValidAdapter(adapter)) return false;
    if (epoch < m_epoch) return false;
    if (epoch != m_epoch) Reset(epoch);
    if (m_adapter)
    {
        if (SameAdapter(*m_adapter, adapter)) return m_mode != Rewards::Mode::Blocked;
        Fault(); return false;
    }
    if (m_mode == Rewards::Mode::Blocked) return false;
    m_adapter = adapter; return true;
}
std::optional<Rewards::Adapter> RewardMailbox::Adapter(uint64_t epoch) const
{
    std::lock_guard lock(m_mutex);
    return epoch == m_epoch && !m_broken ? m_adapter : std::nullopt;
}
void RewardMailbox::Configure(uint64_t epoch, Rewards::Mode mode)
{
    std::lock_guard lock(m_mutex);
    if (!epoch || epoch < m_epoch) return;
    if (epoch != m_epoch) Reset(epoch);
    if (m_mode == Rewards::Mode::Blocked)
    {
        if (mode == Rewards::Mode::Negotiating && !m_broken && !m_token && m_events.empty()) m_mode = mode;
        return;
    }
    if (mode == Rewards::Mode::Offline && (m_mode == Rewards::Mode::Active || m_token || !m_events.empty()))
        mode = Rewards::Mode::Blocked; // late native/Papyrus events from an abandoned shared world are not solo awards
    if (mode == Rewards::Mode::Active && !m_adapter) { Fault(); return; }
    m_mode = mode;
}
Rewards::Mode RewardMailbox::Mode() const { std::lock_guard lock(m_mutex); return m_mode; }
Rewards::Submission RewardMailbox::Submit(uint64_t epoch, const Rewards::Source& source)
{
    std::lock_guard lock(m_mutex);
    if (epoch != m_epoch || source.WorldEpoch != epoch) return Rewards::Submission::Blocked;
    if (m_mode == Rewards::Mode::Offline && !source.InboundToken) return Rewards::Submission::PassThrough;
    if (!m_adapter || m_mode == Rewards::Mode::Blocked) { Fault(); return Rewards::Submission::Blocked; }
    if (source.InboundToken)
        return source.InboundToken == m_token && m_token ? Rewards::Submission::PassThrough : Rewards::Submission::Blocked;
    if (!ValidSource(source) || !(m_adapter->Kinds & (1u << source.Kind))) { Fault(); return Rewards::Submission::Blocked; }
    for (const auto& event : m_events) if (event.Source == source) return Rewards::Submission::Queued;
    if (m_events.size() >= kCapacity || m_sequence == std::numeric_limits<uint64_t>::max())
    { Fault(); return Rewards::Submission::Blocked; }
    m_events.push_back({++m_sequence, source}); return Rewards::Submission::Queued;
}
std::vector<RewardMailbox::Event> RewardMailbox::Take(uint64_t epoch)
{
    std::lock_guard lock(m_mutex);
    if (epoch != m_epoch || m_mode != Rewards::Mode::Active) return {};
    std::vector<Event> out; out.swap(m_events); return out;
}
bool RewardMailbox::BeginApplication(uint64_t epoch, uint64_t token)
{
    std::lock_guard lock(m_mutex);
    if (epoch != m_epoch || m_mode != Rewards::Mode::Active || !m_adapter || !token || m_token) return false;
    m_token = token; m_claimed = false; m_completion.reset(); return true;
}
bool RewardMailbox::ClaimApplication(uint64_t epoch, uint64_t token)
{
    std::lock_guard lock(m_mutex);
    if (epoch != m_epoch || m_mode != Rewards::Mode::Active || !token || token != m_token || m_claimed || m_completion) return false;
    m_claimed = true; return true;
}
bool RewardMailbox::CanApply(uint64_t epoch, uint64_t token) const
{
    std::lock_guard lock(m_mutex);
    return epoch == m_epoch && m_mode == Rewards::Mode::Active && token && token == m_token && m_claimed && !m_completion;
}
bool RewardMailbox::Complete(uint64_t epoch, const Rewards::Completion& completion)
{
    std::lock_guard lock(m_mutex);
    if (epoch != m_epoch || completion.WorldEpoch != epoch || !m_token || completion.Token != m_token || m_mode != Rewards::Mode::Active) return false;
    const bool applied = completion.State == Rewards::Result::Applied;
    if (completion.State == Rewards::Result::Pending || completion.State > Rewards::Result::Uncertain || completion.FinalXP < 0 ||
        completion.FinalXP > RewardLedger::kMaxFinalXP || (applied && completion.Pools != (Rewards::Wallet | Rewards::Engine)) ||
        (!applied && (completion.FinalXP || completion.Pools))) { Fault(); return false; }
    if (m_completion)
    {
        if (*m_completion == completion) return true;
        Fault(); return false;
    }
    m_completion = completion; return true;
}
std::optional<Rewards::Completion> RewardMailbox::Completion(uint64_t epoch, uint64_t token) const
{
    std::lock_guard lock(m_mutex);
    return epoch == m_epoch && token == m_token && m_mode == Rewards::Mode::Active ? m_completion : std::nullopt;
}
bool RewardMailbox::FinishApplication(uint64_t epoch, uint64_t token)
{
    std::lock_guard lock(m_mutex);
    if (epoch != m_epoch || !token || token != m_token || !m_completion) return false;
    m_completion.reset(); m_token = 0; m_claimed = false; return true;
}
bool RewardMailbox::ConsumeFault() { std::lock_guard lock(m_mutex); const bool fault = m_fault; m_fault = false; return fault; }
bool RewardMailbox::HasPending() const { std::lock_guard lock(m_mutex); return m_token || !m_events.empty(); }
void RewardMailbox::Block() { std::lock_guard lock(m_mutex); Fault(); }
std::optional<int32_t> PersonalRewardXP(int32_t base, double multiplier, uint32_t quantum) noexcept
{
    if (base <= 0 || base > RewardLedger::kMaxBaseXP || !std::isfinite(multiplier) || multiplier < 0 || !quantum || quantum > 1000) return {};
    const long double value = std::floor(static_cast<long double>(base) * multiplier / quantum) * quantum;
    if (!std::isfinite(value) || value > RewardLedger::kMaxFinalXP) return {};
    return static_cast<int32_t>(value);
}
} // namespace Coop
