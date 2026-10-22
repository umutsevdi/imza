#include "app/flows.h"
#include "common/types.h"
#include "loopback_http.h"
#include "network/network.h"
#include "network/sse_parse.h"
#include "test_state.h"
#include "tools/skills.h"
#include "workspace/review_subsystem.h"
#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace {

// stop() must not depend on a request arriving to unblock accept().
TEST_CASE("loopback server stops without waiting for a connection")
{
    imza::test::LoopbackHttpServer server;
    server.handle = [](imza::test::detail::Socket fd,
                        const imza::test::LoopbackRequest&) {
        imza::test::LoopbackHttpServer::respond(
            fd, "HTTP/1.1 204 No Content", { }, "");
    };
    REQUIRE(server.start());

    const auto started = std::chrono::steady_clock::now();
    server.stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::seconds(2));
}

TEST_CASE("parse_api_error classifies provider error response shapes")
{
    struct Scenario {
        const char* name;
        const char* body;
        imza::Status expected_status;
        const char* expected_message;
    };
    const std::vector<Scenario> scenarios {
        { "OpenAI error object",
            R"({"error":{"message":"Rate limit reached","type":"requests"}})",
            imza::Status::RATE_LIMITED, "Rate limit reached" },
        { "Anthropic error object",
            R"({"type":"error","error":{"type":"rate_limit_error","message":"Number of requests too high"}})",
            imza::Status::RATE_LIMITED, "Number of requests too high" },
        { "string error with budget keyword",
            R"({"error":"insufficient credits"})",
            imza::Status::BUDGET_EXCEEDED, "insufficient credits" },
        { "top-level message fallback",
            R"({"message":"billing problem detected"})",
            imza::Status::BUDGET_EXCEEDED, "billing problem detected" },
        { "non-error JSON body", R"({"choices":[]})", imza::Status::OK, "" },
        { "non-JSON body", "<html>oops</html>", imza::Status::OK, "" },
    };

    for (const auto& scenario : scenarios) {
        CAPTURE(scenario.name);
        std::string message;
        CHECK(imza::parse_api_error(scenario.body, message)
            == scenario.expected_status);
        CHECK(message == scenario.expected_message);
    }
}

TEST_CASE("OpenAI parse turns mid-stream error blocks into ERROR events")
{
    const auto p = imza::get_provider(imza::Route { });

    imza::ParseState state;
    std::vector<imza::StreamEvent> outs;
    p.parse(state, "",
        R"({"error":{"message":"Provider had an incident","code":502}})", outs);
    REQUIRE(outs.size() == 1);
    CHECK(outs[0].kind == imza::StreamEvent::Kind::ERROR);
    CHECK(outs[0].error == imza::Status::API_ERROR);
    CHECK(outs[0].text == "Provider had an incident");
}

// One scripted response over the shared loopback fixture: the stream client
// opens exactly one connection per call.
struct ScriptedApi {
    imza::test::LoopbackHttpServer server;

    ScriptedApi(
        std::string status, std::vector<std::string> headers, std::string body)
    {
        server.handle
            = [status = std::move(status), headers = std::move(headers),
                  body = std::move(body)](imza::test::detail::Socket fd,
                  const imza::test::LoopbackRequest&) {
                  // respond() takes an initializer_list; build the wire
                  // directly so the header vector can stay dynamic.
                  std::string wire = status + "\r\nConnection: close\r\n";
                  for (const std::string& header : headers) {
                      wire += header + "\r\n";
                  }
                  wire += "Content-Length: " + std::to_string(body.size())
                      + "\r\n\r\n" + body;
                  std::size_t sent = 0;
                  while (sent < wire.size()) {
                      const int now = imza::test::detail::send_bytes(
                          fd, wire.data() + sent, wire.size() - sent);
                      if (now <= 0) {
                          return;
                      }
                      sent += static_cast<std::size_t>(now);
                  }
              };
    }

    std::string url() const { return server.base_url(); }
};

TEST_CASE("stream reports rate limit, retry-after and provider message")
{
    ScriptedApi api("HTTP/1.1 429 Too Many Requests",
        { "Content-Type: application/json", "Retry-After: 7" },
        R"({"error":{"message":"Rate limit exceeded"}})");
    REQUIRE(api.server.start());

    imza::Route route;
    route.endpoint = api.url() + "/chat/completions";
    route.api      = api.url();
    route.api_key  = "k";

    imza::ChatRequest req;
    req.model = "gpt-4o";

    std::vector<imza::StreamEvent> events;
    int retry_after       = 0;
    const imza::Status st = imza::stream(
        route, req, [&](const imza::StreamEvent& ev) { events.push_back(ev); },
        &retry_after);

    CHECK(st == imza::Status::RATE_LIMITED);
    CHECK(retry_after == 7);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == imza::StreamEvent::Kind::ERROR);
    CHECK(events[0].error == imza::Status::RATE_LIMITED);
    CHECK(events[0].text == "Rate limit exceeded");
}

TEST_CASE("stream emits CONNECTED then parses SSE on success")
{
    ScriptedApi api("HTTP/1.1 200 OK", { "Content-Type: text/event-stream" },
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n"
        "data: [DONE]\n\n");
    REQUIRE(api.server.start());

    imza::Route route;
    route.endpoint = api.url() + "/chat/completions";
    route.api_key  = "k";

    imza::ChatRequest req;
    req.model = "gpt-4o";

    std::vector<imza::StreamEvent> events;
    const imza::Status st = imza::stream(
        route, req, [&](const imza::StreamEvent& ev) { events.push_back(ev); });

    CHECK(st == imza::Status::OK);
    REQUIRE(events.size() == 3);
    CHECK(events[0].kind == imza::StreamEvent::Kind::CONNECTED);
    CHECK(events[1].kind == imza::StreamEvent::Kind::CONTENT_DELTA);
    CHECK(events[1].text == "Hi");
    CHECK(events[2].kind == imza::StreamEvent::Kind::DONE);
}

TEST_CASE("abort_all_transfers cancels an in-flight request promptly")
{
    // A handler that reads the request but never answers: the transfer
    // would ride out its full timeout without the abort.
    imza::test::LoopbackHttpServer server;
    server.handle = [](imza::test::detail::Socket fd,
                        const imza::test::LoopbackRequest&) {
        char byte = 0;
        (void)imza::test::detail::receive_bytes(fd, &byte, 1);
    };
    REQUIRE(server.start());

    std::atomic<bool> finished { false };
    std::thread request([&] {
        std::string body;
        long code = 0;
        const imza::Status st
            = imza::http_get(server.base_url() + "/slow", { }, 30, body, &code);
        (void)st;
        finished.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto started = std::chrono::steady_clock::now();
    imza::abort_all_transfers();
    const auto deadline
        = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!finished.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(finished.load());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(elapsed < std::chrono::seconds(5));
    request.join();
    server.stop();
}

} // namespace
