// Host tests for the dependency-free gameplay math.
//
// These do NOT need xmake, MSVC, EnTT, TiltedCore or a copy of Skyrim: they are
// plain C++17 with a 40-line harness, so they can run in this sandbox, on a
// Linux server and in CI. That matters because every other test in this
// repository needs the full build environment.
//
//   g++ -std=c++17 -O2 -Wall -Wextra -Werror Code/tests/pure/pure_tests.cpp -o pure_tests && ./pure_tests
//   (or: tools/run_pure_tests.sh)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <Gameplay/ActorValueIds.h>
#include <Gameplay/HealthScaling.h>
#include <Gameplay/SaveSession.h>
#include <Gameplay/XpAward.h>
#include <Gameplay/Progression.h>
#include <limits>
#include <Gameplay/RateLimiter.h>
#include <Gameplay/XpShare.h>

namespace
{
int g_failures = 0;
int g_checks = 0;
const char* g_currentCase = "";

void BeginCase(const char* acName)
{
    g_currentCase = acName;
    std::printf("-- %s\n", acName);
}

void Check(bool aCondition, const char* acExpression, int aLine)
{
    ++g_checks;
    if (aCondition)
        return;

    ++g_failures;
    std::printf("   FAIL [%s] line %d: %s\n", g_currentCase, aLine, acExpression);
}

void CheckFloat(float aActual, float aExpected, float aTolerance, const char* acExpression, int aLine)
{
    ++g_checks;
    if (std::fabs(aActual - aExpected) <= aTolerance)
        return;

    ++g_failures;
    std::printf("   FAIL [%s] line %d: %s (actual %f, expected %f)\n", g_currentCase, aLine, acExpression, aActual, aExpected);
}

#define CHECK(expr) Check((expr), #expr, __LINE__)
#define CHECK_FLOAT(actual, expected, tol) CheckFloat((actual), (expected), (tol), #actual, __LINE__)

using namespace Gameplay;

void TestHealthMultiplier()
{
    BeginCase("HealthScaling::MultiplierFor - requirement N+1");

    // The product requirement, literally.
    CHECK_FLOAT(HealthScaling::MultiplierFor(1), 1.0f, 0.0f);
    CHECK_FLOAT(HealthScaling::MultiplierFor(2), 3.0f, 0.0f);
    CHECK_FLOAT(HealthScaling::MultiplierFor(3), 4.0f, 0.0f);
    CHECK_FLOAT(HealthScaling::MultiplierFor(4), 5.0f, 0.0f);

    // Degenerate inputs must never produce 0 or a negative multiplier, otherwise
    // every NPC would die instantly or become unkillable.
    CHECK_FLOAT(HealthScaling::MultiplierFor(0), 1.0f, 0.0f);

    // Clamped to the safety cap.
    CHECK(HealthScaling::MultiplierFor(1000) <= HealthScaling::kMaxMultiplier);
    CHECK(HealthScaling::MultiplierFor(1000) >= 1.0f);
    CHECK_FLOAT(HealthScaling::MultiplierFor(HealthScaling::kMaxPlayersConsidered), 32.0f, 0.0f);
}

void TestScaledMax()
{
    BeginCase("HealthScaling::ScaledMax");

    CHECK_FLOAT(HealthScaling::ScaledMax(100.0f, 3.0f), 300.0f, 0.001f);
    CHECK_FLOAT(HealthScaling::ScaledMax(100.0f, 1.0f), 100.0f, 0.001f);

    // Dead / zeroed actors stay zero: scaling must not resurrect them.
    CHECK_FLOAT(HealthScaling::ScaledMax(0.0f, 4.0f), 0.0f, 0.0f);
    CHECK_FLOAT(HealthScaling::ScaledMax(-5.0f, 4.0f), -5.0f, 0.0f);

    // A multiplier below 1 must never shrink an actor.
    CHECK_FLOAT(HealthScaling::ScaledMax(100.0f, 0.5f), 100.0f, 0.001f);
    CHECK_FLOAT(HealthScaling::ScaledMax(100.0f, 0.0f), 100.0f, 0.001f);
}

void TestRescaleCurrent()
{
    BeginCase("HealthScaling::RescaleCurrent keeps the health ratio");

    // Full health stays full.
    CHECK_FLOAT(HealthScaling::RescaleCurrent(100.0f, 100.0f, 300.0f), 300.0f, 0.001f);

    // 25% health stays 25%.
    CHECK_FLOAT(HealthScaling::RescaleCurrent(25.0f, 100.0f, 300.0f), 75.0f, 0.001f);

    // Half health of a boss keeps 50% so percentage phase triggers still fire.
    CHECK_FLOAT(HealthScaling::RescaleCurrent(5000.0f, 10000.0f, 40000.0f), 20000.0f, 0.01f);

    // Out of range inputs are clamped, never negative, never above max.
    CHECK_FLOAT(HealthScaling::RescaleCurrent(-50.0f, 100.0f, 300.0f), 0.0f, 0.0f);
    CHECK_FLOAT(HealthScaling::RescaleCurrent(150.0f, 100.0f, 300.0f), 300.0f, 0.001f);

    // Division by zero guards.
    CHECK_FLOAT(HealthScaling::RescaleCurrent(10.0f, 0.0f, 300.0f), 0.0f, 0.0f);
    CHECK_FLOAT(HealthScaling::RescaleCurrent(10.0f, 100.0f, 0.0f), 0.0f, 0.0f);
}

void TestShouldScale()
{
    BeginCase("HealthScaling::ShouldScale filters");

    HealthScaling::ActorFlags npc{};
    CHECK(HealthScaling::ShouldScale(npc));

    HealthScaling::ActorFlags player{};
    player.IsPlayer = true;
    CHECK(!HealthScaling::ShouldScale(player));

    HealthScaling::ActorFlags dead{};
    dead.IsDead = true;
    CHECK(!HealthScaling::ShouldScale(dead));

    HealthScaling::ActorFlags summon{};
    summon.IsPlayerSummon = true;
    CHECK(!HealthScaling::ShouldScale(summon));
    HealthScaling::Options scaleSummons{};
    scaleSummons.ScaleSummons = true;
    CHECK(HealthScaling::ShouldScale(summon, scaleSummons));

    // Mounts belong to a player: never scaled, not even with every option on.
    HealthScaling::ActorFlags mount{};
    mount.IsMount = true;
    CHECK(!HealthScaling::ShouldScale(mount, scaleSummons));

    // Dragons are ordinary hostile enemies for this feature.
    HealthScaling::ActorFlags dragon{};
    dragon.IsDragon = true;
    CHECK(HealthScaling::ShouldScale(dragon));

    // A dead player is still a player.
    HealthScaling::ActorFlags deadPlayer{};
    deadPlayer.IsPlayer = true;
    deadPlayer.IsDead = true;
    CHECK(!HealthScaling::ShouldScale(deadPlayer, scaleSummons));
}

void TestComputeTarget()
{
    BeginCase("HealthScaling::ComputeTarget");

    float newMax = 0.f;
    float newCurrent = 0.f;

    // Healthy NPC jumps from 100 to 300 with 2 players, keeping 40% health.
    CHECK(HealthScaling::ComputeTarget(100.0f, 40.0f, 100.0f, 3.0f, newMax, newCurrent));
    CHECK_FLOAT(newMax, 300.0f, 0.001f);
    CHECK_FLOAT(newCurrent, 120.0f, 0.001f);

    // Already scaled: no change requested (idempotent, this runs every second).
    CHECK(!HealthScaling::ComputeTarget(100.0f, 120.0f, 300.0f, 3.0f, newMax, newCurrent));

    // Scaling back to single player restores the base maximum.
    CHECK(HealthScaling::ComputeTarget(100.0f, 120.0f, 300.0f, 1.0f, newMax, newCurrent));
    CHECK_FLOAT(newMax, 100.0f, 0.001f);
    CHECK_FLOAT(newCurrent, 40.0f, 0.001f);

    // Zero base max: nothing to do, caller keeps the actor untouched.
    CHECK(!HealthScaling::ComputeTarget(0.0f, 0.0f, 0.0f, 3.0f, newMax, newCurrent));
}

void TestSequenceTracker()
{
    BeginCase("XpShare::SequenceTracker");

    XpShare::SequenceTracker tracker;
    CHECK(tracker.Accept(0));   // first report of a session
    CHECK(tracker.Accept(1));
    CHECK(tracker.Accept(2));

    // Replays and out-of-order reports are rejected.
    CHECK(!tracker.Accept(2));
    CHECK(!tracker.Accept(1));

    // A reconnect restarts the counter: sequence 0 must be accepted again.
    CHECK(tracker.Accept(0));
    CHECK(tracker.LastSequence() == 0);
    CHECK(tracker.Accept(1));

    // Big jumps are still valid (packets may be dropped).
    CHECK(tracker.Accept(1000));
    CHECK(!tracker.Accept(999));

    tracker.Reset();
    CHECK(!tracker.HasSequence());
    CHECK(tracker.Accept(5));
}

void TestAmountWindow()
{
    BeginCase("XpShare::AmountWindow");

    XpShare::AmountWindow window(5000);

    CHECK(window.ShouldBroadcast(100, 1000));  // first report
    CHECK(!window.ShouldBroadcast(100, 1200)); // same amount, same event
    CHECK(window.ShouldBroadcast(250, 1300));  // a different event

    // After the window expires the same amount may be a genuinely new event.
    CHECK(window.ShouldBroadcast(100, 7000));

    // Clock going backwards (server restart of the timer) must not break it.
    window.Reset();
    CHECK(window.ShouldBroadcast(70, 1000));
    CHECK(window.ShouldBroadcast(70, 500));
    CHECK(window.Size() <= 1);

    // Zero window: nothing is deduplicated.
    XpShare::AmountWindow noWindow(0);
    CHECK(noWindow.ShouldBroadcast(5, 100));
    CHECK(noWindow.ShouldBroadcast(5, 101));

    // The stored list is bounded.
    XpShare::AmountWindow bounded(60'000);
    for (uint32_t i = 0; i < XpShare::kMaxTrackedAmounts * 2; ++i)
        bounded.ShouldBroadcast(i + 1, 1000);

    CHECK(bounded.Size() <= XpShare::kMaxTrackedAmounts);
}

void TestSanitise()
{
    BeginCase("XpShare::SanitiseAmount");

    CHECK(XpShare::SanitiseAmount(0) == 0);
    CHECK(XpShare::SanitiseAmount(250) == 250);
    CHECK(XpShare::SanitiseAmount(0xFFFFFFFF) == XpShare::kMaxSingleReport);
    CHECK(XpShare::SanitiseAmount(XpShare::kMaxSingleReport + 1) == XpShare::kMaxSingleReport);
}

void TestXpEchoFilter()
{
    BeginCase("XpShare::EchoFilter");

    using Gameplay::XpShare::EchoFilter;

    EchoFilter filter;

    // Nothing recorded: everything is applied.
    CHECK(!filter.ShouldSuppress(100, 1000));

    // A locally applied award is not applied again right away.
    filter.Record(100, 1000);
    CHECK(filter.ShouldSuppress(100, 1200));
    CHECK(filter.Size() == 0); // consumed, so a later identical award still works

    // Outside the window it is a genuinely new award.
    filter.Record(100, 1000);
    CHECK(!filter.ShouldSuppress(100, 1000 + EchoFilter::kEchoWindowMs + 1));

    // A different amount is a different event.
    filter.Record(100, 2000);
    CHECK(!filter.ShouldSuppress(250, 2100));

    // Stale entries are dropped, memory stays bounded.
    for (uint32_t i = 0; i < 100; ++i)
        filter.Record(5, 5000 + i);
    CHECK(filter.Size() <= EchoFilter::kMaxEntries);

    // A clock that jumps backwards must not resurrect an old award.
    filter.Reset();
    filter.Record(42, 9000);
    CHECK(!filter.ShouldSuppress(42, 100));

    // Zero is never a real award.
    CHECK(filter.ShouldSuppress(0, 0));
}

void TestSavePolicyParsing()
{
    BeginCase("SaveSession::ParsePolicy");

    using Gameplay::SaveSession::Policy;

    // Defaults when there is no file at all.
    const Policy defaults = Gameplay::SaveSession::ParsePolicy("");
    CHECK(defaults.StopSessionOnLoad);
    CHECK(defaults.IndexSaves);
    CHECK(defaults.PollIntervalMs == 5000);

    const std::string ini = R"([Save]
; a comment
bStopSessionOnLoad = no
bIndexSaves = TRUE
uPollIntervalMs = 1500
[Other]
bStopSessionOnLoad = 1
)";

    const Policy policy = Gameplay::SaveSession::ParsePolicy(ini);
    CHECK(!policy.StopSessionOnLoad); // the [Other] section must not count
    CHECK(policy.IndexSaves);
    CHECK(policy.PollIntervalMs == 1500);

    // Case insensitive keys, whitespace, CRLF.
    const Policy crlf = Gameplay::SaveSession::ParsePolicy("[save]\r\n  BSTOPSESSIONONLOAD = 0  \r\n");
    CHECK(!crlf.StopSessionOnLoad);

    // Garbage values keep the previous value instead of zeroing the policy.
    const Policy garbage = Gameplay::SaveSession::ParsePolicy("[Save]\nuPollIntervalMs = nonsense\nbIndexSaves = maybe\n");
    CHECK(garbage.PollIntervalMs == 5000);
    CHECK(garbage.IndexSaves);

    // The polling interval is clamped.
    CHECK(Gameplay::SaveSession::ParsePolicy("[Save]\nuPollIntervalMs = 1\n").PollIntervalMs == 1000);
    CHECK(Gameplay::SaveSession::ParsePolicy("[Save]\nuPollIntervalMs = 9999999\n").PollIntervalMs == 60000);
}

void TestSaveIndexJson()
{
    BeginCase("SaveSession JSON index line");

    Gameplay::SaveSession::SaveIndexEntry entry;
    entry.File = "Save 12 - Dragonborn.ess";
    entry.CreatedUtc = "2026-10-22T02:00:00Z";
    entry.Server = "127.0.0.1:10578";
    entry.Character = "Dovah";
    entry.SessionId = 42;
    entry.SizeBytes = 1234;
    entry.InSession = true;

    const std::string line = Gameplay::SaveSession::FormatSaveIndexLine(entry);
    CHECK(line.find("\"sessionId\":42") != std::string::npos);
    CHECK(line.find("\"inSession\":true") != std::string::npos);
    CHECK(line.find("\"sizeBytes\":1234") != std::string::npos);
    CHECK(line.front() == '{');
    CHECK(line.back() == '}');

    // Quotes and backslashes must not break the JSON.
    CHECK(Gameplay::SaveSession::EscapeJson("a\"b\\c") == "a\\\"b\\\\c");

    // Control characters become escapes.
    CHECK(Gameplay::SaveSession::EscapeJson("a\nb") == "a\\nb");
    // Note: "\x01b" would be a single 0x1B escape, so build the string explicitly.
    const std::string control = std::string("a") + '\x01' + "b";
    CHECK(Gameplay::SaveSession::EscapeJson(control) == "a\\u0001b");

    // A file name with a quote stays valid JSON.
    Gameplay::SaveSession::SaveIndexEntry weird;
    weird.File = "Save \"quoted\".ess";
    const std::string weirdLine = Gameplay::SaveSession::FormatSaveIndexLine(weird);
    CHECK(weirdLine.find("\\\"quoted\\\"") != std::string::npos);
}

void TestSaveSettleTracker()
{
    BeginCase("SaveSession::SettleTracker");

    using Gameplay::SaveSession::FileStamp;
    using Gameplay::SaveSession::SettleTracker;

    SettleTracker tracker;

    // Files that existed before the session are primed and never reported.
    tracker.Prime("Quicksave0.ess", FileStamp{1000, 111});
    CHECK(!tracker.Observe("Quicksave0.ess", FileStamp{1000, 111}, 0));
    CHECK(!tracker.Observe("Quicksave0.ess", FileStamp{1000, 111}, 100000));

    // A new file is only reported once its stamp stops changing.
    CHECK(!tracker.Observe("Save1.ess", FileStamp{10, 200}, 1000));
    CHECK(!tracker.Observe("Save1.ess", FileStamp{20, 201}, 1500)); // still growing
    CHECK(!tracker.Observe("Save1.ess", FileStamp{30, 202}, 2000)); // still growing
    CHECK(!tracker.Observe("Save1.ess", FileStamp{30, 202}, 3000)); // 1000 ms of quiet
    CHECK(tracker.Observe("Save1.ess", FileStamp{30, 202}, 3600));  // settled
    CHECK(!tracker.Observe("Save1.ess", FileStamp{30, 202}, 99999)); // never twice

    // Overwriting the same slot (manual save) is reported again.
    CHECK(!tracker.Observe("Save1.ess", FileStamp{31, 300}, 100000));
    CHECK(tracker.Observe("Save1.ess", FileStamp{31, 300}, 102000));

    // A file removed and recreated looks like a new file.
    tracker.Forget("Save1.ess");
    CHECK(!tracker.Observe("Save1.ess", FileStamp{31, 300}, 200000));
    CHECK(tracker.Observe("Save1.ess", FileStamp{31, 300}, 202000));

    CHECK(tracker.TrackedCount() == 2);

    // Two different files do not influence each other.
    SettleTracker second;
    CHECK(!second.Observe("a.ess", FileStamp{1, 1}, 0));
    CHECK(second.Observe("a.ess", FileStamp{1, 1}, 2000));
    CHECK(!second.Observe("b.ess", FileStamp{1, 1}, 2000));
}

void TestXpAward()
{
    BeginCase("XpAward::Apply");

    using Gameplay::XpAward::Apply;
    using Gameplay::XpAward::kMaxSingleAward;
    using Gameplay::XpAward::kMaxTotalXp;

    const auto normal = Apply(1000, 250);
    CHECK(!normal.Ignored && !normal.Clamped);
    CHECK(normal.Previous == 1000);
    CHECK(normal.Total == 1250);
    CHECK(normal.Applied == 250);

    // Negative totals are sanitised, not propagated.
    const auto negativeTotal = Apply(-500, 10);
    CHECK(negativeTotal.Previous == 0);
    CHECK(negativeTotal.Total == 10);

    // Zero/negative awards are ignored (the vanilla pipeline never subtracts).
    CHECK(Apply(100, 0).Ignored);
    CHECK(Apply(100, -50).Ignored);
    CHECK(Apply(100, -50).Total == 100);

    // Singles above the cap are rejected instead of being silently truncated.
    CHECK(Apply(0, kMaxSingleAward + 1).Ignored);
    CHECK(Apply(0, kMaxSingleAward).Total == kMaxSingleAward);

    // The total is clamped and the applied amount reflects the clamp.
    const auto clamped = Apply(kMaxTotalXp - 100, 500);
    CHECK(clamped.Clamped);
    CHECK(clamped.Total == kMaxTotalXp);
    CHECK(clamped.Applied == 100);

    const auto atLimit = Apply(kMaxTotalXp, 1);
    CHECK(atLimit.Clamped);
    CHECK(atLimit.Applied == 0);

    // No int64 overflow for absurd inputs.
    const auto overflowish = Apply(2'000'000'000, kMaxSingleAward);
    CHECK(overflowish.Total == kMaxTotalXp);
    CHECK(overflowish.Clamped);
    CHECK(overflowish.Previous == kMaxTotalXp);
    CHECK(overflowish.Applied == 0); // never negative

    BeginCase("XpAward::ShouldAnnounceLevelUp");
    CHECK(Gameplay::XpAward::ShouldAnnounceLevelUp(100, 100.0f));
    CHECK(Gameplay::XpAward::ShouldAnnounceLevelUp(150, 100.0f));
    CHECK(!Gameplay::XpAward::ShouldAnnounceLevelUp(99, 100.0f));
    CHECK(!Gameplay::XpAward::ShouldAnnounceLevelUp(1000, 0.0f));   // oracle unavailable
    CHECK(!Gameplay::XpAward::ShouldAnnounceLevelUp(1000, -1.0f));
}

void TestProgressionCatchUp()
{
    BeginCase("Progression::ComputeCatchUp");

    using Gameplay::Progression::ComputeCatchUp;
    using Gameplay::Progression::kMaxTotalXp;

    // A returning player who lost progress is topped up to the server total.
    CHECK(ComputeCatchUp(5000, 1000) == 4000);

    // Exactly equal, or ahead (played single player in between): nothing.
    CHECK(ComputeCatchUp(5000, 5000) == 0);
    CHECK(ComputeCatchUp(5000, 9000) == 0);

    // Nothing known: never invent progress.
    CHECK(ComputeCatchUp(0, 0) == 0);
    CHECK(ComputeCatchUp(0, 500) == 0);
    CHECK(ComputeCatchUp(-100, 0) == 0);

    // Garbage from a client is treated as zero, not as a negative deficit.
    CHECK(ComputeCatchUp(1000, -50) == 0);

    // The catch-up is clamped to the same ceiling as everything else.
    const int32_t big = ComputeCatchUp(kMaxTotalXp, 0);
    CHECK(big == kMaxTotalXp);

    // abs(INT32_MIN) must not overflow.
    CHECK(ComputeCatchUp(100, -2147483647 - 1) == 0);
}

void TestProgressionRecord()
{
    BeginCase("Progression ledger records");

    using Gameplay::Progression::FormatRecord;
    using Gameplay::Progression::Ledger;
    using Gameplay::Progression::Merge;
    using Gameplay::Progression::ParseRecord;
    using Gameplay::Progression::PlayerRecord;

    PlayerRecord record;
    record.Username = "Dovah \"the\" Bold";
    record.RfabXp = 1234;
    record.Level = 17;
    record.UpdatedUnixMs = 1756000000123ull;
    record.CellBaseId = 0x1A26F;
    record.CellModId = 0;
    record.WorldSpaceBaseId = 0x3C;
    record.WorldSpaceModId = 0;
    record.PositionX = 1234.5f;
    record.PositionY = -42.25f;
    record.PositionZ = 0.0f;

    const std::string line = FormatRecord(record);

    PlayerRecord parsed;
    CHECK(ParseRecord(line, parsed));
    CHECK(parsed.Username == record.Username);
    CHECK(parsed.RfabXp == 1234);
    CHECK(parsed.Level == 17);
    CHECK(parsed.UpdatedUnixMs == record.UpdatedUnixMs);
    CHECK(parsed.CellBaseId == 0x1A26F);
    CHECK(parsed.WorldSpaceBaseId == 0x3C);
    CHECK(parsed.PositionX == 1234.5f);
    CHECK(parsed.PositionY == -42.25f);

    // A control character in a username survives the round trip.
    PlayerRecord weird;
    weird.Username = std::string("a") + '\t' + "b";
    weird.RfabXp = 1;
    PlayerRecord weirdParsed;
    CHECK(ParseRecord(FormatRecord(weird), weirdParsed));
    CHECK(weirdParsed.Username == weird.Username);

    // Malformed input is rejected, not half parsed.
    PlayerRecord rejected;
    CHECK(!ParseRecord("", rejected));
    CHECK(!ParseRecord("{}", rejected));
    CHECK(!ParseRecord("{\"user\":\"\"}", rejected));
    CHECK(!ParseRecord("{\"xp\":5}", rejected));
    CHECK(!ParseRecord("not json at all", rejected));
    CHECK(!ParseRecord("{\"user\":7}", rejected));
    CHECK(!ParseRecord("{\"user\":\"a\",}", rejected));
    CHECK(!ParseRecord("{\"user\":\"a\"} trailing", rejected));

    // Unknown keys are ignored (forward compatibility).
    CHECK(ParseRecord("{\"user\":\"a\",\"future\":\"value\"}", rejected));
    CHECK(rejected.Username == "a");

    BeginCase("Progression::Merge");
    PlayerRecord stored;
    stored.Username = "a";
    stored.RfabXp = 5000;
    stored.Level = 10;
    stored.UpdatedUnixMs = 200;

    PlayerRecord current;
    current.Username = "a";
    current.RfabXp = 1000;
    current.Level = 8;
    current.UpdatedUnixMs = 300;
    current.CellBaseId = 0x99;

    // Experience never goes backwards, the newest snapshot wins for the rest.
    const PlayerRecord merged = Merge(stored, current);
    CHECK(merged.RfabXp == 5000);
    CHECK(merged.Level == 8);
    CHECK(merged.CellBaseId == 0x99);

    // An older observation must not overwrite a newer snapshot.
    PlayerRecord older = current;
    older.UpdatedUnixMs = 100;
    older.CellBaseId = 0x11;
    const PlayerRecord keptNewer = Merge(stored, older);
    CHECK(keptNewer.CellBaseId == 0x0); // stored snapshot (200) wins over 100
    CHECK(keptNewer.RfabXp == 5000);

    // Empty inputs are handled.
    CHECK(Merge(PlayerRecord{}, current).Username == "a");
    CHECK(Merge(stored, PlayerRecord{}).RfabXp == 5000);

    BeginCase("Progression::Ledger");
    Ledger ledger;
    CHECK(ledger.Size() == 0);

    ledger.Upsert(record);
    CHECK(ledger.Size() == 1);
    CHECK(ledger.Find("Dovah \"the\" Bold") != nullptr);
    CHECK(ledger.Find("nobody") == nullptr);

    PlayerRecord second;
    second.Username = "Second";
    second.RfabXp = 10;
    ledger.Upsert(second);

    // Same username merges instead of duplicating.
    PlayerRecord third = second;
    third.RfabXp = 20;
    third.UpdatedUnixMs = 5;
    ledger.Upsert(third);
    CHECK(ledger.Size() == 2);
    CHECK(ledger.Find("Second")->RfabXp == 20);

    const std::string text = ledger.Serialize();
    Ledger restored;
    CHECK(restored.Deserialize(text) == 2);
    CHECK(restored.Find("Second")->RfabXp == 20);
    CHECK(restored.Find("Dovah \"the\" Bold") != nullptr);

    // A file with one broken line keeps the good ones.
    Ledger partial;
    CHECK(partial.Deserialize("garbage\n{\"user\":\"ok\",\"xp\":5}\n\n") == 1);
    CHECK(partial.Find("ok")->RfabXp == 5);

    // Re-serialising a restored ledger is stable.
    CHECK(restored.Serialize() == text);
}

void TestHealthRestore()
{
    BeginCase("HealthScaling::ComputeRestore");

    using Gameplay::HealthScaling::ComputeRestore;

    // Exact base known: it is preferred over the multiplier.
    const auto exact = ComputeRestore(300.0f, 100.0f, 3.0f);
    CHECK(exact.ShouldRestore);
    CHECK(exact.Base == 100.0f);

    // Late joiner: only the multiplier is known.
    const auto byMultiplier = ComputeRestore(400.0f, 0.0f, 4.0f);
    CHECK(byMultiplier.ShouldRestore);
    CHECK(byMultiplier.Base == 100.0f);

    // Not scaled: nothing to do.
    CHECK(!ComputeRestore(100.0f, 0.0f, 1.0f).ShouldRestore);
    CHECK(!ComputeRestore(100.0f, 100.0f, 3.0f).ShouldRestore);

    // Remembered base cannot make things worse.
    CHECK(!ComputeRestore(100.0f, 200.0f, 1.0f).ShouldRestore);
    CHECK(!ComputeRestore(100.0f, -5.0f, 1.0f).ShouldRestore);

    // Garbage in: no restore, no NaN propagation.
    CHECK(!ComputeRestore(0.0f, 100.0f, 3.0f).ShouldRestore);
    CHECK(!ComputeRestore(-100.0f, 100.0f, 3.0f).ShouldRestore);
    CHECK(!ComputeRestore(std::numeric_limits<float>::quiet_NaN(), 100.0f, 3.0f).ShouldRestore);
    CHECK(!ComputeRestore(300.0f, 0.0f, std::numeric_limits<float>::quiet_NaN()).ShouldRestore);
    CHECK(!ComputeRestore(300.0f, 0.0f, 0.5f).ShouldRestore); // never scales up

    // A multiplier of 1 with no remembered base is a no-op.
    CHECK(!ComputeRestore(300.0f, 0.0f, 1.0f).ShouldRestore);
}

void TestRateLimiter()
{
    BeginCase("TokenBucket");

    using Gameplay::TokenBucket;

    // A default constructed bucket only allows a single action.
    TokenBucket unconfigured;
    CHECK(!unconfigured.IsConfigured());
    CHECK(unconfigured.TryConsume(0));
    CHECK(!unconfigured.TryConsume(0));

    // Burst of 3, then one token per second.
    TokenBucket bucket(3, 1);
    CHECK(bucket.IsConfigured());

    // The first call only starts the clock: a client that connects to a server
    // running for hours must not receive a huge burst.
    CHECK(bucket.TryConsume(1'000'000)); // token 1 (from the initial fill)
    CHECK(bucket.TryConsume(1'000'000)); // token 2
    CHECK(bucket.TryConsume(1'000'000)); // token 3
    CHECK(!bucket.TryConsume(1'000'000));
    CHECK(bucket.Rejected() == 1);

    // A second later exactly one token is back.
    CHECK(bucket.TryConsume(1'001'000));
    CHECK(!bucket.TryConsume(1'001'000));

    // Partial seconds do not accumulate into a token ...
    CHECK(!bucket.TryConsume(1'001'500));

    // ... but they are not lost either: 600 ms + 400 ms is one token.
    CHECK(bucket.TryConsume(1'002'000));

    // Idle time refills at most to capacity.
    CHECK(bucket.TryConsume(2'000'000));
    CHECK(bucket.TryConsume(2'000'000));
    CHECK(bucket.TryConsume(2'000'000));
    CHECK(!bucket.TryConsume(2'000'000));
    CHECK(bucket.Available() == 0);

    // A clock that jumps backwards refills instead of blocking forever.
    CHECK(bucket.TryConsume(10));

    // Refill rate of zero means "burst only".
    TokenBucket burstOnly(2, 0);
    CHECK(burstOnly.TryConsume(0));
    CHECK(burstOnly.TryConsume(0));
    CHECK(!burstOnly.TryConsume(0));
    CHECK(!burstOnly.TryConsume(10'000'000));

    // Reconfiguration refills.
    burstOnly.Configure(1, 10);
    CHECK(burstOnly.TryConsume(0));
    CHECK(!burstOnly.TryConsume(0));

    // 64 bit arithmetic: a very long idle period must not overflow.
    TokenBucket longIdle(5, 4);
    CHECK(longIdle.TryConsume(0));
    CHECK(longIdle.TryConsume(0xFFFF'FFFF'FFFFull));
    CHECK(longIdle.Available() <= 5);
}

void TestActorValueIds()
{
    BeginCase("ActorValue ids are the Skyrim SE ones");

    // Mirrors Code/client/Games/Skyrim/Forms/ActorValueInfo.h; if anyone edits
    // one of the two, this test and the client static_assert disagree.
    CHECK(ActorValue::kHealth == 24);
    CHECK(ActorValue::kMagicka == 25);
    CHECK(ActorValue::kStamina == 26);
}

} // namespace

int main()
{
    std::printf("RFAB Online - pure gameplay tests\n");

    TestHealthMultiplier();
    TestScaledMax();
    TestRescaleCurrent();
    TestShouldScale();
    TestComputeTarget();
    TestSequenceTracker();
    TestAmountWindow();
    TestSavePolicyParsing();
    TestSaveIndexJson();
    TestSaveSettleTracker();
    TestXpAward();
    TestXpEchoFilter();
    TestProgressionCatchUp();
    TestProgressionRecord();
    TestRateLimiter();
    TestHealthRestore();
    TestSanitise();
    TestActorValueIds();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);

    return g_failures == 0 ? 0 : 1;
}
