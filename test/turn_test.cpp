#include <doctest/doctest.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app/flows.h"
#include "conversation/session.h"
#include "providers/catalog.h"
#include "test_fs.h"
#include "test_state.h"
#include "turn/prompt_store.h"

namespace imza {

TEST_CASE("base system prompt without environment")
{
    const PromptStore prompts;
    const std::string prompt = build_system_prompt(prompts, nullptr, nullptr);
    CHECK(prompt.find("imza") != std::string::npos);
    CHECK(prompt.find("PLAN") != std::string::npos);
    CHECK(prompt.find("BUILD") != std::string::npos);
    CHECK(prompt.find("<runtime-mode name=") == std::string::npos);
    CHECK(prompt.find("<env>") == std::string::npos);
    CHECK(prompt.find("Available tools") == std::string::npos);
}

TEST_CASE("system prompt embeds the environment block")
{
    SystemEnvironment sys;
    sys.os_name          = "Linux";
    sys.os_version       = "6.8";
    sys.default_shell    = "/bin/bash";
    sys.package_managers = { "apt", "snap" };
    sys.today            = "Fri Aug 28 2026";
#ifdef _WIN32
    sys.temporary_directory = "C:\\Users\\me\\AppData\\Local\\Temp\\imza";
#else
    sys.temporary_directory = "/tmp/imza";
#endif

    const PromptStore prompts;
    const std::string prompt = build_system_prompt(prompts, &sys, nullptr);
    CHECK(prompt.find("<env>") != std::string::npos);
    CHECK(prompt.find("Current Directory") != std::string::npos);
    CHECK(prompt.find("Operating System: Linux 6.8") != std::string::npos);
    CHECK(prompt.find("/bin/bash") != std::string::npos);
    CHECK(prompt.find("apt, snap") != std::string::npos);
#ifdef _WIN32
    CHECK(prompt.find("Temporary Directory: "
                      "C:\\Users\\me\\AppData\\Local\\Temp\\imza")
        != std::string::npos);
#else
    CHECK(prompt.find("Temporary Directory: /tmp/imza") != std::string::npos);
#endif
    CHECK(prompt.find("Fri Aug 28 2026") != std::string::npos);
    CHECK(prompt.find("</env>") != std::string::npos);
}

TEST_CASE("system prompt embeds workspace instructions when present")
{
    SystemEnvironment sys;
    sys.os_name       = "Linux";
    sys.default_shell = "/bin/bash";
    sys.today         = "Fri Aug 28 2026";
    WorkspaceEnvironment ws;
    ws.working_directory = std::filesystem::temp_directory_path();
    ws.instruction = InstructionFile { "AGENTS.md", "# Rules\nBe terse." };

    const PromptStore prompts;
    const std::string prompt = build_system_prompt(prompts, &sys, &ws);
    CHECK(prompt.find("<instructions source=\"AGENTS.md\">")
        != std::string::npos);
    CHECK(prompt.find("Be terse.") != std::string::npos);
    CHECK(prompt.find("</instructions>") != std::string::npos);
}

TEST_CASE("system prompt appends configured instructions in order")
{
    SystemEnvironment sys;
    sys.os_name       = "Linux";
    sys.default_shell = "/bin/bash";
    sys.today         = "Fri Aug 28 2026";
    WorkspaceEnvironment ws;
    ws.working_directory = std::filesystem::temp_directory_path();
    ws.instruction       = InstructionFile { "AGENTS.md", "base rules" };
    ws.extra_instructions.push_back(
        InstructionFile { "docs/style.md", "first extra" });
    ws.extra_instructions.push_back(
        InstructionFile { "/home/me/rules.md", "second extra" });

    const PromptStore prompts;
    const std::string prompt = build_system_prompt(prompts, &sys, &ws);
    const std::size_t base   = prompt.find("base rules");
    const std::size_t first  = prompt.find("first extra");
    const std::size_t second = prompt.find("second extra");
    REQUIRE(base != std::string::npos);
    REQUIRE(first != std::string::npos);
    REQUIRE(second != std::string::npos);
    CHECK(base < first);
    CHECK(first < second);
    CHECK(prompt.find("<instructions source=\"docs/style.md\">")
        != std::string::npos);
    CHECK(prompt.find("<instructions source=\"/home/me/rules.md\">")
        != std::string::npos);
}

TEST_CASE("system prompt advertises active skills and hides denied skills")
{
    SystemEnvironment sys;
    sys.global_skills.clear();
    sys.global_skills.emplace("docs",
        Skill { "docs", "Write documentation", "/tmp/docs/SKILL.md",
            Skill::Scope::GLOBAL, std::nullopt });
    sys.global_skills.emplace("secret",
        Skill { "secret", "Hidden", "/tmp/secret/SKILL.md",
            Skill::Scope::GLOBAL, std::nullopt });
    Config config;
    config.global_skills["docs"]   = SkillPolicy::ALLOW;
    config.global_skills["secret"] = SkillPolicy::DENY;
    const PromptStore prompts;
    const std::string prompt
        = build_system_prompt(prompts, &sys, nullptr, &config);
    CHECK(
        prompt.find("docs [global]: Write documentation") != std::string::npos);
    CHECK(prompt.find("secret [global]") == std::string::npos);
    CHECK(prompt.find("`skill` tool") != std::string::npos);
}

TEST_CASE("subagent prompts enforce role policies")
{
    const PromptStore prompts;

    const std::string research = build_subagent_system_prompt(
        prompts, nullptr, nullptr, SubagentRole::RESEARCH);
    CHECK(research.find("Imza subagent") != std::string::npos);
    CHECK(research.find("fresh context") != std::string::npos);
    CHECK(research.find("Work read-only") != std::string::npos);
    CHECK(research.find("implementation plan") != std::string::npos);
    CHECK(research.find("# Todo list") == std::string::npos);
    CHECK(research.find("interactive CLI coding agent") == std::string::npos);

    const std::string builder = build_subagent_system_prompt(
        prompts, nullptr, nullptr, SubagentRole::BUILDER);
    CHECK(builder.find("may modify files") != std::string::npos);
    CHECK(builder.find("keep changes focused") != std::string::npos);
    CHECK(builder.find("Work read-only") == std::string::npos);

    CHECK(build_subagent_system_prompt(
        prompts, nullptr, nullptr, SubagentRole::BASIC)
            .empty());
}

TEST_CASE("subagent prompt retains workspace context")
{
    SystemEnvironment sys;
    sys.os_name       = "Linux";
    sys.default_shell = "/bin/bash";
    sys.today         = "Tue Sep 1 2026";
    sys.global_skills.emplace("docs",
        Skill { "docs", "Write documentation", "/tmp/docs/SKILL.md",
            Skill::Scope::GLOBAL, std::nullopt });
    WorkspaceEnvironment ws;
    ws.working_directory = std::filesystem::temp_directory_path();
    ws.instruction = InstructionFile { "AGENTS.md", "Use project rules." };

    const PromptStore prompts;
    const std::string prompt = build_subagent_system_prompt(
        prompts, &sys, &ws, SubagentRole::BUILDER);
    CHECK(prompt.find("Operating System: Linux") != std::string::npos);
    CHECK(prompt.find("<instructions source=\"AGENTS.md\">")
        != std::string::npos);
    CHECK(prompt.find("Use project rules.") != std::string::npos);
    CHECK(
        prompt.find("docs [global]: Write documentation") != std::string::npos);
    CHECK(prompt.find("$skill-name") == std::string::npos);
}

namespace {

    // Data home whose catalog gives the test model a tiny context so the
    // automatic compaction threshold is reachable from a stubbed usage event.
    struct CompactionHome : imza::test::IsolatedDataHome {
        CompactionHome()
        {
            imza::CachedModel m;
            m.cost_input  = 1.0;
            m.cost_output = 2.0;
            m.context     = 1000;
            imza::CachedProvider provider;
            provider.name   = "Test";
            provider.models = { { "m", std::move(m) } };
            imza::Catalog catalog;
            catalog.fetched_at = std::time(nullptr);
            catalog.providers  = { { "test", provider } };
            std::filesystem::create_directories(imza_dir());
            std::ignore
                = imza::save_catalog(imza_dir() / "presets.json", catalog);
        }
    };

    imza::StreamEvent usage(std::uint64_t prompt)
    {
        imza::Usage u;
        u.prompt = prompt;
        u.total  = prompt;
        return imza::make_usage_event(u);
    }

    std::size_t summarize_requests(const std::vector<imza::ChatRequest>& reqs)
    {
        std::size_t count = 0;
        for (const auto& req : reqs) {
            if (req.messages.size() == 2
                && req.messages[0].content.find("Summarize")
                    != std::string::npos) {
                ++count;
            }
        }
        return count;
    }

} // namespace

TEST_CASE("automatic compaction runs in the background after the turn")
{
    CompactionHome home;
    imza::test::AgentEnv env;
    REQUIRE(
        env.state->providers->pricing_for("m", "test").context_limit == 1000);
    imza::test::AgentEnv* ep = &env;
    env.stream = [ep](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) -> imza::Status {
        ep->requests.push_back(req);
        if (summarize_requests({ req }) == 1) {
            cb(imza::make_delta_event("bg summary"));
            cb(imza::make_done_event());
            return imza::Status::OK;
        }
        cb(imza::make_connected_event());
        cb(imza::make_delta_event("reply"));
        cb(usage(900));
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    // The first two turns run uncompacted: compaction needs enough
    // history and fires only after a completed turn.
    imza::submit(*env.state, "first question");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    imza::submit(*env.state, "second question");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));

    // Compaction finishes in the background after the turn went idle.
    REQUIRE(env.pump.wait_for(
        [&] { return !env.session->snapshot().compacted_summary.empty(); }));
    CHECK(env.session->snapshot().compacted_summary == "bg summary");
    CHECK(summarize_requests(env.requests) == 1);

    // The next turn's history leads with the committed summary.
    const std::size_t before = env.requests.size();
    imza::submit(*env.state, "third question");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    REQUIRE(env.requests.size() > before);
    const auto& next = env.requests[before];
    REQUIRE(next.messages.size() > 1);
    CHECK(next.messages[1].content.find("bg summary") != std::string::npos);
}

TEST_CASE("an unknown context window still triggers compaction")
{
    // A model with no reported window (local server, injected provider)
    // would otherwise never compact; a conservative fallback window keeps
    // it working.
    CompactionHome home;
    imza::test::AgentEnv env;
    imza::test::AgentEnv* ep = &env;
    env.stream = [ep](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) -> imza::Status {
        ep->requests.push_back(req);
        if (summarize_requests({ req }) == 1) {
            cb(imza::make_delta_event("bg summary"));
            cb(imza::make_done_event());
            return imza::Status::OK;
        }
        cb(imza::make_connected_event());
        cb(imza::make_delta_event("reply"));
        // Above the fallback window's 80% trigger.
        cb(usage(14000));
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    // Point the active model at one the catalog does not carry.
    REQUIRE(env.state->providers->select_model(
        imza::ModelChoice { "test", "unknown-window-model" }));
    REQUIRE(
        env.state->providers->context_limit_for("unknown-window-model", "test")
        == 0);

    imza::submit(*env.state, "first question");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    imza::submit(*env.state, "second question");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));

    REQUIRE(env.pump.wait_for(
        [&] { return !env.session->snapshot().compacted_summary.empty(); }));
    CHECK(env.session->snapshot().compacted_summary == "bg summary");
}

TEST_CASE("a later compaction summarizes only post-boundary messages")
{
    CompactionHome home;
    imza::test::AgentEnv env;
    imza::test::AgentEnv* ep = &env;
    env.stream = [ep](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) -> imza::Status {
        ep->requests.push_back(req);
        if (summarize_requests({ req }) == 1) {
            // Echo the summarized transcript back so the test can tell
            // what range the summarizer saw.
            cb(imza::make_delta_event(req.messages[1].content));
            cb(imza::make_done_event());
            return imza::Status::OK;
        }
        cb(imza::make_connected_event());
        cb(imza::make_delta_event("reply"));
        cb(usage(900));
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "first question");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    imza::submit(*env.state, "second question");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    REQUIRE(env.pump.wait_for(
        [&] { return !env.session->snapshot().compacted_summary.empty(); }));
    const std::string first = env.session->snapshot().compacted_summary;
    CHECK(first.find("second question") != std::string::npos);

    imza::submit(*env.state, "third question");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    REQUIRE(env.pump.wait_for([&] {
        const std::string summary = env.session->snapshot().compacted_summary;
        return summary != first && !summary.empty();
    }));
    const std::string merged = env.session->snapshot().compacted_summary;
    // The merged context keeps the prior generation, but the summarizer
    // never re-read it.
    CHECK(merged.find("third question") != std::string::npos);
    CHECK(merged.find("earlier-compactions") != std::string::npos);
    CHECK(merged.find("second question") != std::string::npos);
    REQUIRE(summarize_requests(env.requests) == 2);
}

TEST_CASE("manual /compact reports an in-progress compaction")
{
    CompactionHome home;
    imza::test::AgentEnv env;
    env.stream = [](const imza::ChatRequest& req,
                     const imza::StreamCallback& cb) -> imza::Status {
        if (summarize_requests({ req }) == 1) {
            // Stalled summarizer: keeps the compaction in flight.
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            cb(imza::make_delta_event("slow summary"));
            cb(imza::make_done_event());
            return imza::Status::OK;
        }
        cb(imza::make_connected_event());
        cb(imza::make_delta_event("reply"));
        cb(imza::make_done_event());
        return imza::Status::OK;
    };

    imza::submit(*env.state, "hello");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));

    imza::run_slash(*env.state, "/compact");
    CHECK(env.state->runner->compaction_active());
    imza::run_slash(*env.state, "/compact");
    CHECK(env.session->error() == "Compaction already in progress.");
    REQUIRE(env.pump.wait_for(
        [&] { return !env.state->runner->compaction_active(); }));
    CHECK(env.session->snapshot().compacted_summary == "slow summary");
}

namespace {

    using AgentEnv = imza::test::AgentEnv;

    // True when an assistant turn with exactly `markdown` is in the session.
    bool assistant_markdown_present(
        const AgentEnv& env, std::string_view markdown)
    {
        const auto items = env.session->items();
        for (const auto& it : *items) {
            if (const auto* a = std::get_if<imza::AssistantTurn>(&it)) {
                if (a->markdown == markdown) {
                    return true;
                }
            }
        }
        return false;
    }

} // namespace

TEST_CASE("controller retries rate-limited requests and then completes")
{
    AgentEnv env;
    auto round   = std::make_shared<int>(0);
    AgentEnv* ep = &env;
    env.stream   = [ep, round](const imza::ChatRequest& req,
                       const imza::StreamCallback& cb) -> imza::Status {
        ep->requests.push_back(req);
        if ((*round)++ == 0) {
            cb(imza::make_error_event(imza::Status::RATE_LIMITED, "slow down"));
            return imza::Status::RATE_LIMITED;
        }
        cb(imza::make_connected_event());
        cb(imza::make_delta_event("recovered"));
        cb(imza::make_done_event());
        return imza::Status::OK;
    };
    imza::submit(*env.state, "hello");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    CHECK(env.requests.size() == 2);
    CHECK(env.session->error().empty());
    CHECK(assistant_markdown_present(env, "recovered"));
}

TEST_CASE("controller does not retry budget errors")
{
    AgentEnv env;
    auto round   = std::make_shared<int>(0);
    AgentEnv* ep = &env;
    env.stream   = [ep, round](const imza::ChatRequest& req,
                       const imza::StreamCallback& cb) -> imza::Status {
        ep->requests.push_back(req);
        cb(imza::make_error_event(
            imza::Status::BUDGET_EXCEEDED, "insufficient credits"));
        return imza::Status::BUDGET_EXCEEDED;
    };
    imza::submit(*env.state, "hello");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    CHECK(env.requests.size() == 1);
    CHECK(env.session->error()
        == "Out of budget / insufficient credits: insufficient credits.");
}

TEST_CASE("controller retries stalled connections before any data arrives")
{
    AgentEnv env;
    auto round   = std::make_shared<int>(0);
    AgentEnv* ep = &env;
    env.stream   = [ep, round](const imza::ChatRequest& req,
                       const imza::StreamCallback& cb) -> imza::Status {
        ep->requests.push_back(req);
        if ((*round)++ == 0) {
            cb(imza::make_connected_event());
            cb(imza::make_error_event(
                imza::Status::TIMEOUT, "Operation too slow"));
            return imza::Status::TIMEOUT;
        }
        cb(imza::make_connected_event());
        cb(imza::make_delta_event("recovered"));
        cb(imza::make_done_event());
        return imza::Status::OK;
    };
    imza::submit(*env.state, "hello");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    CHECK(env.requests.size() == 2);
    CHECK(env.session->error().empty());
    CHECK(assistant_markdown_present(env, "recovered"));
}

TEST_CASE("controller does not retry a stall after content arrived")
{
    AgentEnv env;
    AgentEnv* ep = &env;
    env.stream   = [ep](const imza::ChatRequest& req,
                       const imza::StreamCallback& cb) -> imza::Status {
        ep->requests.push_back(req);
        cb(imza::make_connected_event());
        cb(imza::make_delta_event("partial"));
        cb(imza::make_error_event(imza::Status::TIMEOUT, "Operation too slow"));
        return imza::Status::TIMEOUT;
    };
    imza::submit(*env.state, "hello");
    REQUIRE(env.pump.wait_for([&] { return imza::test::idle(*env.session); }));
    CHECK(env.requests.size() == 1);
    CHECK(env.session->error() == "Timed out: Operation too slow.");
}

} // namespace imza
