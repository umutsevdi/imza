#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "conversation/persistence.h"
#include "conversation/session.h"
#include "network/chat.h"
#include "network/network.h"
#include "providers/pricing.h"
#include "test_fs.h"
#include "test_helpers.h"

namespace {

using imza::Usage;
using imza::UsageEntry;

imza::StreamEvent usage_event(std::uint64_t total, double completion = 0)
{
    Usage u;
    u.prompt     = total - completion;
    u.completion = completion;
    u.total      = total;
    return imza::make_usage_event(u);
}

imza::ModelPricing test_pricing()
{
    imza::ModelPricing pricing;
    pricing.input_per_1k  = 1.0;
    pricing.output_per_1k = 2.0;
    return pricing;
}

UsageEntry make_entry(const std::string& provider, const std::string& model,
    std::uint64_t total, double cost)
{
    UsageEntry entry;
    entry.provider     = provider;
    entry.model        = model;
    entry.usage.total  = total;
    entry.usage.prompt = total;
    entry.cost         = cost;
    return entry;
}

// Saves `source`, requires exactly one saved session, loads it back.
std::filesystem::path roundtrip(imza::Session& source, imza::Session& loaded)
{
    REQUIRE(imza::save_session(source) == imza::Status::OK);
    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 1);
    imza::LoadedSession loaded_session;
    REQUIRE(imza::read_session(saved.front().path, loaded_session)
        == imza::Status::OK);
    loaded.restore(std::move(loaded_session.snapshot));
    return saved.front().path;
}

} // namespace

TEST_CASE("usage accumulates per turn and in the analytics log")
{
    imza::test::IsolatedDataHome home;
    imza::Session session;
    session.begin_send("hello");
    session.append_assistant("m", "off");
    // Two requests in one turn (a tool loop).
    session.apply(usage_event(100), test_pricing());
    session.apply(usage_event(150, 50), test_pricing());
    session.finish_session("");
    REQUIRE(session.status_view().totals.total == 250);

    const auto items = session.items();
    REQUIRE(items->size() == 2);
    const auto* turn = std::get_if<imza::AssistantTurn>(&(*items)[1]);
    REQUIRE(turn != nullptr);
    REQUIRE(turn->usage.size() == 2);
    std::uint64_t tokens = 0;
    for (const auto& entry : turn->usage) {
        tokens += entry.usage.total;
    }
    CHECK(tokens == 250);
    CHECK(turn->finished_at != std::chrono::system_clock::time_point { });

    const auto log = session.usage_log();
    REQUIRE(log.size() == 2);
    CHECK(log.back().model == "m");
}

TEST_CASE("save and load round-trips usage state")
{
    imza::test::IsolatedDataHome home;
    imza::Session source;
    source.set_title("usage");
    source.begin_send("hello");
    source.append_assistant("m", "off");
    source.apply(usage_event(100, 100), test_pricing());
    source.finish_session("");
    source.set_entry_provider("test");
    REQUIRE(source.status_view().totals.total == 100);

    imza::Session loaded;
    roundtrip(source, loaded);

    CHECK(loaded.status_view().totals.total == 100);
    CHECK(loaded.status_view().last.total == 100);
    CHECK(loaded.status_view().total_cost == source.status_view().total_cost);
    CHECK(loaded.status_view().total_cost > 0.0);

    const auto log = loaded.usage_log();
    REQUIRE(log.size() == 1);
    CHECK(log.front().usage.total == 100);

    const auto items = loaded.items();
    REQUIRE(items->size() == 2);
    const auto* turn = std::get_if<imza::AssistantTurn>(&(*items)[1]);
    REQUIRE(turn != nullptr);
    REQUIRE(turn->usage.size() == 1);
    CHECK(turn->finished_at != std::chrono::system_clock::time_point { });
}

TEST_CASE("subagent usage folds into the parent exactly once")
{
    imza::test::IsolatedDataHome home;
    imza::Session parent;
    parent.begin_send("delegate");
    parent.append_assistant("m", "off");
    parent.apply(usage_event(20), test_pricing());

    const std::uint64_t cost_before = parent.status_view().total_cost;

    parent.add_usage({ make_entry("test", "child", 1000, 0.5) });
    parent.add_usage({ make_entry("test", "child", 1000, 0.5) });

    const auto view = parent.status_view();
    CHECK(view.totals.total == 2020);
    CHECK(view.total_cost > cost_before);
    REQUIRE(parent.usage_log().size() == 3);

    const auto items = parent.items();
    const auto* turn = std::get_if<imza::AssistantTurn>(&items->back());
    REQUIRE(turn != nullptr);
    CHECK(turn->usage.size() == 3);

    // A drain hands the entries over; draining again yields nothing, so a
    // re-fold cannot double-count. Totals move with the log: a saved
    // summary of the drained session stays consistent with it.
    auto drained = parent.drain_usage();
    REQUIRE(drained.size() == 3);
    CHECK(parent.drain_usage().empty());
    const auto drained_view = parent.status_view();
    CHECK(drained_view.totals.total == 0);
    CHECK(drained_view.total_cost == 0.0);
}

TEST_CASE("index usage summary aggregates across sessions")
{
    imza::test::IsolatedDataHome home;
    imza::Session first;
    first.set_title("one");
    first.begin_send("a");
    first.append_assistant("m1", "off");
    first.set_entry_provider("alpha");
    first.apply(usage_event(100), test_pricing());
    first.finish_session("");
    REQUIRE(imza::save_session(first) == imza::Status::OK);

    imza::Session second;
    second.set_title("two");
    second.begin_send("b");
    second.append_assistant("m2", "off");
    second.set_entry_provider("beta");
    second.apply(usage_event(700), test_pricing());
    second.finish_session("");
    REQUIRE(imza::save_session(second) == imza::Status::OK);

    const auto saved = imza::saved_sessions();
    REQUIRE(saved.size() == 2);
    std::uint64_t indexed_total = 0;
    for (const auto& entry : saved) {
        indexed_total += entry.usage.total;
    }
    CHECK(indexed_total == 800);

    const imza::GlobalUsageSummary summary = imza::global_usage_summary();
    CHECK(summary.total == 800);
    CHECK(summary.total_cost > 0.0);
    // Each session's entry groups under its own provider.
    REQUIRE(summary.by_model.size() == 2);
    std::uint64_t by_model_total = 0;
    for (const auto& row : summary.by_model) {
        by_model_total += row.total;
    }
    CHECK(by_model_total == 800);
    REQUIRE_FALSE(summary.by_day.empty());
    double by_day_total = 0.0;
    for (const auto& row : summary.by_day) {
        by_day_total += row.cost;
        CHECK(row.day.size() == 10);
    }
    CHECK(by_day_total == doctest::Approx(summary.total_cost));

    // The in-memory path agrees with the index-backed one.
    const imza::GlobalUsageSummary from_saved
        = imza::summarize_saved_usage(saved);
    CHECK(from_saved.total == summary.total);
    CHECK(from_saved.total_cost == doctest::Approx(summary.total_cost));
    CHECK(from_saved.by_day == summary.by_day);
}

TEST_CASE("rollup and submission grouping")
{
    // The shared rollup groups entries per (provider, model).
    const std::vector<imza::UsageBreakdownRow> rolled
        = imza::rollup_usage_by_model({ make_entry("alpha", "m1", 100, 0.1),
            make_entry("alpha", "m1", 50, 0.05),
            make_entry("beta", "m2", 700, 0.7) });
    REQUIRE(rolled.size() == 2);
    CHECK(rolled[0].provider == "alpha");
    CHECK(rolled[0].total == 150);
    CHECK(rolled[1].model == "m2");
    CHECK(rolled[1].total == 700);

    // Same-model entries within one day merge into one stack segment.
    imza::UsageEntry entry = make_entry("alpha", "m1", 10, 0.01);
    entry.finished_at      = std::chrono::system_clock::now();
    const imza::GlobalUsageSummary merged
        = imza::summarize_usage({ entry, entry }, "2026-01-01");
    REQUIRE(merged.by_day.size() == 1);
    REQUIRE(merged.by_day.front().by_model.size() == 1);
    CHECK(merged.by_day.front().by_model.front().total == 20);

    // submission_usage groups each conversation turn.
    imza::Session session;
    session.begin_send("a");
    session.append_assistant("m1", "off");
    session.apply(usage_event(100), test_pricing());
    session.begin_send("b");
    session.append_assistant("m2", "off");
    session.apply(usage_event(50), test_pricing());
    const std::vector<imza::SubmissionUsage> groups
        = imza::submission_usage(*session.items());
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].tokens == 100);
    CHECK(groups[0].model == "m1");
    CHECK(groups[1].tokens == 50);
    CHECK(groups[1].model == "m2");
}

TEST_CASE("summaries bucket spend per day, with legacy fallbacks")
{
    imza::test::IsolatedDataHome home;

    // Entries without timestamps land on the fallback day.
    const imza::GlobalUsageSummary legacy = imza::summarize_usage(
        { make_entry("p", "m", 100, 0.25) }, "2026-10-26");
    REQUIRE(legacy.by_day.size() == 1);
    CHECK(legacy.by_day.front().day == "2026-10-26");
    CHECK(legacy.by_day.front().total == 100);
    CHECK(legacy.by_day.front().cost == doctest::Approx(0.25));
    REQUIRE(legacy.by_day.front().by_model.size() == 1);
    CHECK(legacy.by_day.front().by_model.front().model == "m");
    CHECK(legacy.by_day.front().by_model.front().total == 100);

    // Saved sessions whose index entry predates per-day buckets fall
    // back to their save day.
    imza::SavedSession old;
    old.title                            = "old";
    old.saved_at                         = "2026-03-05 08:00:00";
    old.usage.cost                       = 1.5;
    old.usage.total                      = 500;
    const imza::GlobalUsageSummary mixed = imza::summarize_saved_usage({ old });
    REQUIRE(mixed.by_day.size() == 1);
    CHECK(mixed.by_day.front().day == "2026-03-05");
    CHECK(mixed.by_day.front().total == 500);
    CHECK(mixed.by_day.front().cost == doctest::Approx(1.5));
}

TEST_CASE("epoch day buckets remap to the session's save day")
{
    imza::test::IsolatedDataHome home;
    imza::SavedSession poisoned;
    poisoned.title       = "poisoned";
    poisoned.saved_at    = "2026-10-26 23:00:00";
    poisoned.usage.total = 7'050'412;
    poisoned.usage.by_model.push_back(
        { "zai-coding-plan", "glm-5.3", 7'050'412, 0, 7'050'412, 0.0 });
    poisoned.usage.by_day.push_back({ "1970-01-01", 7'050'412, 0.0,
        { { "zai-coding-plan", "glm-5.3", 7'050'412, 0, 7'050'412, 0.0 } } });

    const imza::GlobalUsageSummary summary
        = imza::summarize_saved_usage({ poisoned });
    REQUIRE(summary.by_day.size() == 1);
    CHECK(summary.by_day.front().day == "2026-10-26");
    CHECK(summary.by_day.front().total == 7'050'412);
    REQUIRE(summary.by_day.front().by_model.size() == 1);
    CHECK(summary.by_day.front().by_model.front().total == 7'050'412);
}

TEST_CASE("legacy session files without usage fields load as zero")
{
    imza::test::IsolatedDataHome home;
    std::filesystem::create_directories(imza::sessions_dir());
    // A minimal session file in the old shape: no usage, no usage_log,
    // no finished_at.
    const auto path = imza::sessions_dir() / "legacy.json";
    {
        std::ofstream out(path);
        REQUIRE(out.good());
        out << R"({
  "title": "legacy",
  "saved_at": "2025-01-01 10:00:00",
  "mode": "plan",
  "items": [
    { "type": "user", "text": "hello" },
    { "type": "assistant", "markdown": "hi", "model": "m" }
  ]
})";
    }
    imza::LoadedSession loaded_session;
    REQUIRE(imza::read_session(path, loaded_session) == imza::Status::OK);
    imza::Session loaded;
    loaded.restore(std::move(loaded_session.snapshot));
    const auto view = loaded.status_view();
    CHECK(view.totals.total == 0);
    CHECK(view.total_cost == 0.0);
    CHECK(loaded.usage_log().empty());
    const auto items = loaded.items();
    REQUIRE(items->size() == 2);
    const auto* turn = std::get_if<imza::AssistantTurn>(&(*items)[1]);
    REQUIRE(turn != nullptr);
    CHECK(turn->usage.empty());
    CHECK(turn->finished_at == std::chrono::system_clock::time_point { });
}
