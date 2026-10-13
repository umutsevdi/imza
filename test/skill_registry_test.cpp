#include <doctest/doctest.h>

#include "app/flows.h"
#include "loopback_http.h"
#include "test_fs.h"
#include "test_helpers.h"
#include "test_state.h"
#include "tools/skill_registry.h"
#include "ui/ui.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

using imza::Status;

struct ScopedSkillsUrl {
    // Exports IMZA_SKILLS_URL, restoring the previous value afterwards.
    explicit ScopedSkillsUrl(const std::string& url)
    {
        if (const char* previous = std::getenv("IMZA_SKILLS_URL")) {
            _previous = previous;
            _had      = true;
        }
        ::setenv("IMZA_SKILLS_URL", url.c_str(), 1);
    }

    ~ScopedSkillsUrl()
    {
        if (_had) {
            ::setenv("IMZA_SKILLS_URL", _previous.c_str(), 1);
        } else {
            ::unsetenv("IMZA_SKILLS_URL");
        }
    }

    ScopedSkillsUrl(const ScopedSkillsUrl&)            = delete;
    ScopedSkillsUrl& operator=(const ScopedSkillsUrl&) = delete;

    std::string _previous;
    bool _had = false;
};

TEST_CASE("parse_skill_search reads the skills.sh payload")
{
    const std::string body = R"({
      "query": "react",
      "skills": [
        {"id": "vercel-labs/agent-skills/vercel-react-best-practices",
         "source": "vercel-labs/agent-skills",
         "skillId": "vercel-react-best-practices",
         "name": "vercel-react-best-practices",
         "installs": 791923},
        {"id": "bad/entry"}
      ]
    })";
    const std::vector<imza::RemoteSkill> skills
        = imza::parse_skill_search(body);
    REQUIRE(skills.size() == 1); // the malformed entry is dropped
    CHECK(imza::parse_skill_search("not json").empty());
    CHECK(
        skills[0].id == "vercel-labs/agent-skills/vercel-react-best-practices");
    CHECK(skills[0].source == "vercel-labs/agent-skills");
    CHECK(skills[0].slug == "vercel-react-best-practices");
    CHECK(skills[0].installs == 791923);
}

TEST_CASE("parse_skill_download reads the file list")
{
    const std::string body = R"({
      "files": [
        {"path": "SKILL.md", "contents": "# Skill\n"},
        {"path": "scripts/run.sh", "contents": "echo hi\n"}
      ]
    })";
    const std::vector<imza::RemoteSkillFile> files
        = imza::parse_skill_download(body);
    REQUIRE(files.size() == 2);
    CHECK(files[0].path == "SKILL.md");
    CHECK(files[1].path == "scripts/run.sh");
}

TEST_CASE("install_skill_files writes a bounded, safe file set")
{
    imza::test::TempDir root;
    const std::vector<imza::RemoteSkillFile> files {
        { "SKILL.md", "# Demo\n" },
        { "scripts/run.sh", "echo hi\n" },
    };
    std::string detail;
    CHECK(imza::install_skill_files(root.path, "demo", files, detail)
        == Status::OK);
    CHECK(detail.empty());
    CHECK(std::filesystem::is_regular_file(root.path / "demo" / "SKILL.md"));
    CHECK(std::filesystem::is_regular_file(
        root.path / "demo" / "scripts" / "run.sh"));
}

TEST_CASE("install_skill_files rejects unsafe paths and missing SKILL.md")
{
    imza::test::TempDir root;
    std::string detail;
    CHECK(imza::install_skill_files(
              root.path, "demo", { { "../escape.md", "x" } }, detail)
        == Status::API_ERROR);
    CHECK(imza::install_skill_files(
              root.path, "demo", { { "/etc/passwd", "x" } }, detail)
        == Status::API_ERROR);
    CHECK(imza::install_skill_files(
              root.path, "demo", { { "notes.md", "x" } }, detail)
        == Status::API_ERROR);
    CHECK_FALSE(std::filesystem::exists(root.path / "demo"));
}

TEST_CASE("remove_skill_files deletes the skill directory only")
{
    imza::test::TempDir root;
    const auto dir = root.path / "demo";
    imza::test::write_file(dir / "SKILL.md", "# Demo\n");
    std::string detail;
    CHECK(imza::remove_skill_files(dir / "SKILL.md", detail) == Status::OK);
    CHECK_FALSE(std::filesystem::exists(dir));

    imza::test::write_file(root.path / "other.md", "x");
    CHECK(imza::remove_skill_files(root.path / "other.md", detail)
        == Status::CONFIG_ERROR);
    CHECK(std::filesystem::exists(root.path / "other.md"));
}

TEST_CASE("skill registry searches and installs through the loopback server")
{
    imza::test::allow_loopback_direct();
    imza::test::LoopbackHttpServer server;
    std::string last_query;
    server.handle = [&last_query](
                        int fd, const imza::test::LoopbackRequest& req) {
        if (req.path == "/api/search") {
            last_query = req.query;
            imza::test::LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK",
                { "Content-Type: application/json" },
                R"({"skills":[{"id":"owner/repo/demo","source":"owner/repo",)"
                R"("skillId":"demo","name":"Demo","installs":42}]})");
            return;
        }
        if (req.path == "/api/download/owner/repo/demo") {
            imza::test::LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK",
                { "Content-Type: application/json" },
                R"({"files":[{"path":"SKILL.md","contents":"# Demo\n"}]})");
            return;
        }
        imza::test::LoopbackHttpServer::respond(
            fd, "HTTP/1.1 404 Not Found", { }, "{}");
    };
    REQUIRE(server.start());

    imza::test::TempDir root;
    imza::SkillRegistryHooks hooks;
    hooks.base_url     = server.base_url();
    hooks.install_root = root.path;
    unsigned mutations = 0;
    hooks.on_mutated   = [&mutations] { ++mutations; };
    imza::SkillRegistry registry(std::move(hooks));

    registry.search("react");
    bool searched = false;
    for (int i = 0; i < 500 && !searched; ++i) {
        const auto snap = registry.snapshot();
        if (snap.phase == imza::SkillRegistry::Phase::IDLE
            && !snap.results.empty()) {
            searched = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(searched);
    CHECK(last_query.find("q=react") != std::string::npos);

    const auto snap = registry.snapshot();
    REQUIRE(snap.results.size() == 1);
    CHECK(snap.results[0].slug == "demo");
    CHECK_FALSE(registry.installed("demo"));

    registry.install(snap.results[0]);
    bool installed = false;
    for (int i = 0; i < 500 && !installed; ++i) {
        installed = registry.installed("demo");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(installed);
    CHECK(std::filesystem::is_regular_file(root.path / "demo" / "SKILL.md"));
    CHECK(mutations >= 1);
    CHECK(registry.snapshot().status_ok);

    registry.remove_path(root.path / "demo" / "SKILL.md");
    bool removed = false;
    for (int i = 0; i < 500 && !removed; ++i) {
        removed = !registry.installed("demo");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(removed);
    CHECK(mutations >= 2);
}

TEST_CASE("skills modal searches as you type and installs the choice")
{
    imza::test::allow_loopback_direct();
    imza::test::LoopbackHttpServer server;
    server.handle = [](int fd, const imza::test::LoopbackRequest& req) {
        if (req.path == "/api/search") {
            imza::test::LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK",
                { "Content-Type: application/json" },
                R"({"skills":[{"id":"owner/repo/demo","source":"owner/repo",)"
                R"("skillId":"demo","name":"Demo","installs":1234}]})");
            return;
        }
        if (req.path == "/api/download/owner/repo/demo") {
            imza::test::LoopbackHttpServer::respond(fd, "HTTP/1.1 200 OK",
                { "Content-Type: application/json" },
                R"({"files":[{"path":"SKILL.md","contents":"# Demo\n"}]})");
            return;
        }
        imza::test::LoopbackHttpServer::respond(
            fd, "HTTP/1.1 404 Not Found", { }, "{}");
    };
    REQUIRE(server.start());
    ScopedSkillsUrl scoped(server.base_url());

    imza::test::TempDir root;
    imza::test::IsolatedDataHome data_home;
    ::setenv("XDG_CONFIG_HOME", data_home.root().c_str(), 1);

    imza::test::AgentEnv env;
    imza::SkillsModal payload;
    payload.entries.push_back(
        { "docs", "Write documentation", "", imza::SkillPolicy::ASK });
    imza::enqueue_user_modal(*env.state, payload);
    REQUIRE(env.pump.wait_for([&] {
        return std::holds_alternative<imza::SkillsModal>(env.session->modal());
    }));

    ftxui::Component modal = imza::make_modal(env.state);
    REQUIRE(modal->OnEvent(ftxui::Event::Tab)); // to the search tab
    imza::test::type(modal, "demo");

    bool listed = false;
    for (int i = 0; i < 200 && !listed; ++i) {
        const std::string text = imza::test::to_text(modal->Render(), 80, 20);
        listed                 = text.find("Demo") != std::string::npos;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(listed);

    // A new query clears the previous list immediately: the stale results
    // must not linger while the next search is in flight.
    imza::test::type(modal, "zz");
    const std::string cleared = imza::test::to_text(modal->Render(), 80, 20);
    CHECK(cleared.find("Demo") == std::string::npos);

    // Back to the first query; Enter installs the selected result.
    for (int i = 0; i < 2; ++i) {
        REQUIRE(modal->OnEvent(ftxui::Event::Backspace));
    }
    bool listed_again = false;
    for (int i = 0; i < 200 && !listed_again; ++i) {
        listed_again = imza::test::to_text(modal->Render(), 80, 20).find("Demo")
            != std::string::npos;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(listed_again);
    REQUIRE(modal->OnEvent(ftxui::Event::Return));
    bool installed = false;
    for (int i = 0; i < 200 && !installed; ++i) {
        installed = std::filesystem::is_regular_file(
            data_home.root() / "agents" / "skills" / "demo" / "SKILL.md");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(installed);
    imza::close_modal(*env.state);
}

TEST_CASE("skill registry reports search failures without throwing")
{
    imza::test::allow_loopback_direct();
    imza::test::LoopbackHttpServer server;
    server.handle = [](int fd, const imza::test::LoopbackRequest&) {
        imza::test::LoopbackHttpServer::respond(
            fd, "HTTP/1.1 500 Server Error", { }, "{}");
    };
    REQUIRE(server.start());

    imza::SkillRegistryHooks hooks;
    hooks.base_url = server.base_url();
    imza::SkillRegistry registry(std::move(hooks));

    registry.search("react");
    bool settled = false;
    for (int i = 0; i < 500 && !settled; ++i) {
        if (registry.snapshot().phase == imza::SkillRegistry::Phase::IDLE
            && !registry.snapshot().status.empty()) {
            settled = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(settled);
    CHECK_FALSE(registry.snapshot().status_ok);
    CHECK(registry.snapshot().results.empty());
}

} // namespace
