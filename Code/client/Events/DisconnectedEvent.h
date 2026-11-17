#pragma once

/**
 * @brief Dispatched when the client disconnects from the server.
 */
struct DisconnectedEvent
{
    explicit DisconnectedEvent(bool aForWorldLoad = false) : ForWorldLoad(aForWorldLoad) {}
    bool ForWorldLoad{};
};
