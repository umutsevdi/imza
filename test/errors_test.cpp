#include <doctest/doctest.h>
#include <json/json.h>

#include "app/flows.h"
#include "common/types.h"
#include "network/network.h"
#include "network/sse_parse.h"
#include "tools/skills.h"
#include "workspace/review.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

TEST_CASE("parse_api_error reads OpenAI-style error objects")
{
    std::string msg;
    const imza::Status st = imza::parse_api_error(
        R"({"error":{"message":"Rate limit reached","type":"requests"}})", msg);
    CHECK(st == imza::Status::RATE_LIMITED);
    CHECK(msg == "Rate limit reached");
}

TEST_CASE("parse_api_error reads Anthropic-style error objects")
{
    std::string msg;
    const imza::Status st = imza::parse_api_error(
        R"({"type":"error","error":{"type":"rate_limit_error","message":"Number of requests too high"}})",
        msg);
    CHECK(st == imza::Status::RATE_LIMITED);
    CHECK(msg == "Number of requests too high");
}

TEST_CASE("parse_api_error reads string errors and budget keywords")
{
    std::string msg;
    const imza::Status st
        = imza::parse_api_error(R"({"error":"insufficient credits"})", msg);
    CHECK(st == imza::Status::BUDGET_EXCEEDED);
    CHECK(msg == "insufficient credits");
}

TEST_CASE("parse_api_error falls back to top-level message")
{
    std::string msg;
    const imza::Status st = imza::parse_api_error(
        R"({"message":"billing problem detected"})", msg);
    CHECK(st == imza::Status::BUDGET_EXCEEDED);
    CHECK(msg == "billing problem detected");
}

TEST_CASE("parse_api_error ignores non-error bodies")
{
    std::string msg;
    const imza::Status st = imza::parse_api_error(R"({"choices":[]})", msg);
    CHECK(st == imza::Status::OK);
    CHECK(msg.empty());

    const imza::Status bad = imza::parse_api_error("<html>oops</html>", msg);
    CHECK(bad == imza::Status::OK);
}

TEST_CASE("error_text maps statuses to human strings")
{
    CHECK(imza::error_text(imza::Status::RATE_LIMITED)
        == "Rate limited by provider.");
    CHECK(imza::error_text(imza::Status::BUDGET_EXCEEDED)
        == "Out of budget / insufficient credits.");
    CHECK(imza::error_text(imza::Status::NETWORK_ERROR) == "Network error.");
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

class PostPump {
public:
    imza::PostFn fn()
    {
        return [this](std::function<void()> f) { _push(std::move(f)); };
    }

    void pump()
    {
        for (;;) {
            std::function<void()> f;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (queue_.empty()) {
                    return;
                }
                f = std::move(queue_.front());
                queue_.pop_front();
            }
            f();
        }
    }

    bool wait_for(std::function<bool()> pred)
    {
        for (int i = 0; i < 20000; ++i) {
            pump();
            if (pred()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

private:
    void _push(std::function<void()> f)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(f));
    }

    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
};

imza::Config test_config()
{
    imza::Config cfg;
    imza::Connection conn;
    conn.id          = "test";
    conn.provider_id = "test";
    cfg.providers.push_back(conn);
    cfg.last_used = imza::LastUsed { "test", "m" };
    return cfg;
}

struct AgentEnv {
    PostPump pump;
    std::vector<imza::ChatRequest> requests;
    imza::StreamFn stream;
    std::shared_ptr<imza::ApplicationState> state
        = imza::make_application_state(pump.fn(), test_config(),
            [this](const imza::ChatRequest& req,
                const imza::StreamCallback& cb) { return stream(req, cb); });
    std::shared_ptr<imza::Session> session = state->session;

    AgentEnv()
    {
        REQUIRE(pump.wait_for([&] { return state->environment->ready(); }));
    }
};

bool idle(const imza::Session& st)
{
    return st.phase() == imza::Session::Phase::IDLE && st.modal().index() == 0;
}

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
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    CHECK(env.requests.size() == 2);
    CHECK(env.session->error().empty());
    const auto& items = env.session->items();
    bool found        = false;
    for (const auto& it : items) {
        if (const auto* a = std::get_if<imza::AssistantTurn>(&it)) {
            found = found || a->markdown == "recovered";
        }
    }
    CHECK(found);
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
    REQUIRE(env.pump.wait_for([&] { return idle(*env.session); }));
    CHECK(env.requests.size() == 1);
    CHECK(env.session->error()
        == "Out of budget / insufficient credits: insufficient credits.");
}

struct FakeApi {
    std::string response;
    std::string request;
    int port = 0;
    int fd   = -1;
    std::thread server;

    explicit FakeApi(std::string resp)
        : response(std::move(resp))
    {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE_MESSAGE(fd >= 0, std::strerror(errno));
        sockaddr_in addr { };
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        REQUIRE_MESSAGE(
            ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0,
            std::strerror(errno));
        socklen_t len = sizeof(addr);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
        REQUIRE(::listen(fd, 1) == 0);
        server = std::thread([this] { _serve(); });
    }

    ~FakeApi()
    {
        if (server.joinable()) {
            server.join();
        }
        ::close(fd);
    }

    void _serve()
    {
        const int c = ::accept(fd, nullptr, nullptr);
        if (c < 0) {
            return;
        }
        std::string acc;
        char buf[8192];
        for (;;) {
            const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            acc.append(buf, static_cast<size_t>(n));
            if (acc.find("\r\n\r\n") != std::string::npos) {
                break;
            }
        }
        request     = acc;
        ssize_t off = 0;
        while (off < static_cast<ssize_t>(response.size())) {
            const ssize_t n = ::send(c, response.data() + off,
                response.size() - static_cast<size_t>(off), 0);
            if (n <= 0) {
                break;
            }
            off += n;
        }
        ::shutdown(c, SHUT_WR);
        ::close(c);
    }
};

TEST_CASE("stream reports rate limit, retry-after and provider message")
{
    FakeApi api("HTTP/1.1 429 Too Many Requests\r\n"
                "Content-Type: application/json\r\n"
                "Retry-After: 7\r\n"
                "\r\n"
                R"({"error":{"message":"Rate limit exceeded"}})");

    imza::Route route;
    route.endpoint
        = "http://127.0.0.1:" + std::to_string(api.port) + "/chat/completions";
    route.api     = "http://127.0.0.1:" + std::to_string(api.port);
    route.api_key = "k";

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
    FakeApi api("HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "\r\n"
                "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n"
                "data: [DONE]\n\n");

    imza::Route route;
    route.endpoint
        = "http://127.0.0.1:" + std::to_string(api.port) + "/chat/completions";
    route.api_key = "k";

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

} // namespace
