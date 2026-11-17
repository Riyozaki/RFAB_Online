#pragma once
#include <utility>

namespace Coop
{
// Restore the prior thread-local override, not a literal false: native calls can re-enter us.
template <class T> class ScopedValue
{
public:
    ScopedValue(T& aTarget, T aValue) noexcept : m_target(aTarget), m_previous(aTarget) { m_target = std::move(aValue); }
    ~ScopedValue() noexcept { m_target = std::move(m_previous); }
    ScopedValue(const ScopedValue&) = delete;
    ScopedValue& operator=(const ScopedValue&) = delete;
private:
    T& m_target;
    T m_previous;
};
} // namespace Coop
