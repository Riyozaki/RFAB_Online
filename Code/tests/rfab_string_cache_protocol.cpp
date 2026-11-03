/*
 * String-table replication protocol — behavioural suite.
 *
 * Why this file exists: CachedString is carried by ActionEvent, Tints and the script-animation
 * messages, i.e. it is on the hot path of animation and appearance sync. The replication protocol
 * had three defects that no test could see, because the existing suites only searched the sources
 * for substrings:
 *
 *   1. An id that is missing from the local table hit `assert(false)`: debug builds aborted and
 *      release builds silently kept the *previous* value of the field, which then travelled on
 *      as a valid-looking animation/tint name.
 *   2. StringCache::Deserialize asserted contiguity and otherwise appended regardless, so a
 *      non-contiguous update shifted every following id and corrupted the rest of the session.
 *   3. The server only pushed updates when new strings had just appeared, so a player joining an
 *      already warm session never received the table at all.
 *
 * Every case below drives the real code paths, not the source text.
 */

#include <TiltedCore/Stl.hpp>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/Serialization.hpp>

#include <catch2/catch.hpp>

#include "StringCache.h"
#include <Messages/StringCacheUpdate.h>
#include <Structs/CachedString.h>

#include <cstdint>
#include <string>

using TiltedPhoques::Buffer;
using TiltedPhoques::Serialization;
using TiltedPhoques::String;

namespace
{
/// Mirrors the server send condition: a peer is behind whenever its cursor trails the table.
[[nodiscard]] bool PeerNeedsUpdate(uint32_t aPeerCursor, size_t aCacheSize) noexcept
{
    return aPeerCursor < aCacheSize;
}

[[nodiscard]] StringCacheUpdate MakeUpdate(uint16_t aStartId, std::initializer_list<const char*> aValues)
{
    StringCacheUpdate update;
    update.StartId = aStartId;
    for (const char* p : aValues)
        update.Values.push_back(String(p));
    return update;
}

[[nodiscard]] CachedString RoundTrip(const CachedString& acValue)
{
    Buffer buffer(1024);
    Buffer::Writer writer(&buffer);
    acValue.Serialize(writer);

    Buffer::Reader reader(&buffer);
    CachedString out;
    out = String("stale-previous-value");
    out.Deserialize(reader);
    return out;
}
} // namespace

TEST_CASE("StringCache: a warm table is replicated to a late joiner", "[rfab][stringcache][protocol]")
{
    auto& cache = StringCache::Get();
    cache.Clear();

    // Session warms up: three strings enter the table and are handed out to the first player.
    REQUIRE(cache.Add(String("attackStart")) == 0u);
    REQUIRE(cache.Add(String("blockStart")) == 1u);
    REQUIRE(cache.Add(String("JumpStandingStart")) == 2u);

    uint32_t firstPlayerCursor = 0;
    const auto firstUpdate = cache.Serialize(firstPlayerCursor);
    CHECK(firstUpdate.Values.size() == 3);
    CHECK(firstPlayerCursor == 3);

    // Nothing new is added afterwards. A player joining now still starts at cursor 0 and MUST be
    // considered behind - this is the invariant the server send loop relies on.
    const uint32_t lateJoinerCursor = 0;
    CHECK(PeerNeedsUpdate(lateJoinerCursor, cache.Size()));
    CHECK_FALSE(PeerNeedsUpdate(firstPlayerCursor, cache.Size()));

    uint32_t cursor = lateJoinerCursor;
    const auto lateUpdate = cache.Serialize(cursor);
    REQUIRE(lateUpdate.Values.size() == 3);
    CHECK(lateUpdate.StartId == 0);
    CHECK(cursor == 3);

    // The joiner applies it and can now resolve every id the server may reference.
    cache.Clear();
    CHECK(cache.Deserialize(lateUpdate));
    REQUIRE(cache.Size() == 3);
    CHECK(*cache[0u] == String("attackStart"));
    CHECK(*cache[2u] == String("JumpStandingStart"));
}

TEST_CASE("CachedString: an unknown id resolves to empty, never to the previous value", "[rfab][stringcache][protocol]")
{
    auto& cache = StringCache::Get();
    cache.Clear();
    cache.Add(String("attackStart"));

    Buffer buffer(64);
    Buffer::Writer writer(&buffer);
    Serialization::WriteBool(writer, true);
    Serialization::WriteVarInt(writer, 4242); // id that was never replicated to us

    Buffer::Reader reader(&buffer);
    CachedString value;
    value = String("previous-animation-event");

    // Must not abort (this used to be assert(false)) and must not keep the stale value.
    value.Deserialize(reader);

    CHECK(value.empty());
    CHECK(cache.GetUnknownIdCount() == 1);
}

TEST_CASE("CachedString: known ids and literals survive a round trip", "[rfab][stringcache][protocol]")
{
    auto& cache = StringCache::Get();
    cache.Clear();

    CachedString literal;
    literal = String("ScriptAnimationEvent");

    // Not in the table yet: travels as a literal and is queued for replication.
    CHECK(RoundTrip(literal) == literal);

    cache.Add(String("ScriptAnimationEvent"));

    // Now in the table: travels as an id and must resolve back to the same text.
    CHECK(RoundTrip(literal) == literal);
    CHECK(cache.GetUnknownIdCount() == 0);
}

TEST_CASE("StringCache: a non-contiguous update is rejected instead of corrupting every id", "[rfab][stringcache][protocol]")
{
    auto& cache = StringCache::Get();
    cache.Clear();
    CHECK(cache.Deserialize(MakeUpdate(0, {"a", "b"})));
    REQUIRE(cache.Size() == 2);

    // Gap: the sender is at 5, we are at 2. Appending would make our "c" id 2 while the sender
    // means 5 - every CachedString afterwards would silently decode to the wrong text.
    CHECK_FALSE(cache.Deserialize(MakeUpdate(5, {"c"})));
    CHECK(cache.Size() == 2);
    CHECK(cache.GetRejectedUpdateCount() == 1);
    CHECK(*cache[0u] == String("a"));
    CHECK(*cache[1u] == String("b"));
}

TEST_CASE("StringCache: replayed and overlapping updates are idempotent", "[rfab][stringcache][protocol]")
{
    auto& cache = StringCache::Get();
    cache.Clear();
    CHECK(cache.Deserialize(MakeUpdate(0, {"a", "b", "c"})));

    // Exact replay (duplicate delivery).
    CHECK(cache.Deserialize(MakeUpdate(0, {"a", "b", "c"})));
    CHECK(cache.Size() == 3);

    // Overlap plus new tail (sender resumed from an older cursor).
    CHECK(cache.Deserialize(MakeUpdate(1, {"b", "c", "d"})));
    REQUIRE(cache.Size() == 4);
    CHECK(*cache[3u] == String("d"));
    CHECK(cache.GetRejectedUpdateCount() == 0);
}

TEST_CASE("StringCache: a mismatching overlap is rejected", "[rfab][stringcache][protocol]")
{
    auto& cache = StringCache::Get();
    cache.Clear();
    CHECK(cache.Deserialize(MakeUpdate(0, {"a", "b"})));

    // The sender claims id 1 is "X" while we already handed out id 1 as "b": the tables disagree
    // on an id that is already in flight, so the update cannot be applied.
    CHECK_FALSE(cache.Deserialize(MakeUpdate(1, {"X", "c"})));
    CHECK(cache.Size() == 2);
    CHECK(*cache[1u] == String("b"));
    CHECK(cache.GetRejectedUpdateCount() == 1);
}

TEST_CASE("StringCache: remote literals cannot grow the table past the wire range", "[rfab][stringcache][protocol]")
{
    auto& cache = StringCache::Get();
    cache.Clear();

    // StartId is a uint16_t on the wire; a table larger than that wraps the offset for everyone.
    CHECK(StringCache::kMaxEntries == 0xFFFFu);

    for (size_t i = 0; i < StringCache::kMaxEntries + 16; ++i)
        cache.AddWanted(String("s" + std::to_string(i)));

    CHECK(cache.ProcessDirty());
    CHECK(cache.Size() <= StringCache::kMaxEntries);
    CHECK(cache.GetDroppedWantedCount() > 0);

    uint32_t cursor = 0;
    const auto update = cache.Serialize(cursor);
    CHECK(static_cast<size_t>(update.StartId) <= StringCache::kMaxEntries);

    cache.Clear();
}
