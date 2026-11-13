#pragma once

#include <common/Session/Lifecycle.h>

struct StringCacheUpdate;

/**
 * @brief Caches strings to reduce data usage when sending string data between clients.
 *
 * This service is currently not in use.
 */
struct StringCacheService final : Session::IParticipant
{
    StringCacheService(entt::dispatcher& aDispatcher) noexcept;
    ~StringCacheService() noexcept = default;

    TP_NOCOPYMOVE(StringCacheService);

    // Session::ParticipantId::kStringTable: the table belongs to one server session.
    void OnSessionBegin(const Session::Transition& acTransition) noexcept override;
    void OnSessionEnd(const Session::Transition& acTransition) noexcept override;

    void HandleStringCacheUpdate(const StringCacheUpdate&) noexcept;

private:
    entt::scoped_connection m_stringCacheUpdateConnection;
};
