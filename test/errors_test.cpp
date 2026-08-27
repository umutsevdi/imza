#include <doctest/doctest.h>
#include <json/json.h>

#include "controller.h"
#include "network.h"
#include "types.h"

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
    const ursa::Status st = ursa::parse_api_error(
        R"({"error":{"message":"Rate limit reached","type":"requests"}})",
        msg);
    CHECK(st == ursa::Status::RATE_LIMITED);
    CHECK(msg == "Rate limit reached");
}

TEST_CASE("parse_api_error reads Anthropic-style error objects")
{
    std::string msg;
    const ursa::Status st = ursa::parse_api_error(
        R"({"type":"error","error":{"type":"rate_limit_error","message":"Number of requests too high"}})",
        msg);
    CHECK(st == ursa::Status::RATE_LIMITED);
    CHECK(msg == "Number of requests too high");
}

TEST_CASE("parse_api_error reads string errors and budget keywords")
{
    std::string msg;
    const ursa::Status st
        = ursa::parse_api_error(R"({"error":"insufficient credits"})", msg);
    CHECK(st == ursa::Status::BUDGET_EXCEEDED);
    CHECK(msg == "insufficient credits");
}

TEST_CASE("parse_api_error falls back to top-level message")
{
    std::string msg;
    const ursa::Status st = ursa::parse_api_error(
        R"({"message":"billing problem detected"})", msg);
    CHECK(st == ursa::Status::BUDGET_EXCEEDED);
    CHECK(msg == "billing problem detected");
}

TEST_CASE("parse_api_error ignores non-error bodies")
{
    std::string msg;
    const ursa::Status st
        = ursa::parse_api_error(R"({"choices":[]})", msg);
    CHECK(st == ursa::Status::OK);
    CHECK(msg.empty());

    const ursa::Status bad = ursa::parse_api_error("<html>oops</html>", msg);
    CHECK(bad == ursa::Status::OK);
}

TEST_CASE("error_text maps statuses to human strings")
{
    CHECK(ursa::error_text(ursa::Status::RATE_LIMITED)
        == "rate limited by provider");
    CHECK(ursa::error_text(ursa::Status::BUDGET_EXCEEDED)
        == "out of budget / insufficient credits");
    CHECK(ursa::error_text(ursa::Status::NETWORK_ERROR) == "network error");
}

TEST_CASE("OpenAI parse turns mid-stream error blocks into ERROR events")
{
    const auto p = ursa::get_provider(ursa::Route { });

    ursa::ParseState state;
    std::vector<ursa::StreamEvent> outs;
    p.parse(state, "",
        R"({"error":{"message":"Provider had an incident","code":502}})",
        outs);
    REQUIRE(outs.size() == 1);
    CHECK(outs[0].kind == ursa::StreamEvent::Kind::ERROR);
    CHECK(outs[0].error == ursa::Status::API_ERROR);
    CHECK(outs[0].text == "Provider had an incident");
}

class PostPump {
public:
    ursa::PostFn fn()
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

struct AgentEnv {
    PostPump pump;
    std::vector<ursa::ChatRequest> requests;
    ursa::StreamFn stream;
    std::shared_ptr<ursa::Session> session
        = std::make_shared<ursa::Session>();
    ursa::Controller controller { session, [] {
        ursa::Config cfg;
        ursa::Connection conn;
        conn.id          = "test";
        conn.provider_id = "test";
        cfg.providers.push_back(conn);
        cfg.last_used = ursa::LastUsed { "test", "m" };
        return cfg;
    }(), pump.fn(), [] { },
        [this](const ursa::ChatRequest& req, const ursa::StreamCallback& cb) {
            return stream(req, cb);
        },
        ursa::ToolRegistry { } };
};

bool idle(const ursa::Session& st)
{
    return st.phase() == ursa::Session::Phase::IDLE && st.modal().index() == 0;
}

TEST_CASE("controller retries rate-limited requests and then completes")
{
    AgentEnv env;
    auto round   = std::make_shared<int>(0);
    AgentEnv* ep = &env;
    env.stream   = [ep, round](const ursa::ChatRequest& req,
                      const ursa::StreamCallback& cb) -> ursa::Status {
        ep->requests.push_back(req);
        if ((*round)++ == 0) {
            cb(ursa::make_error_event(
                ursa::Status::RATE_LIMITED, "slow down"));
            return ursa::Status::RATE_LIMITED;
        }
        cb(ursa::make_connected_event());
        cb(ursa::make_delta_event("recovered"));
        cb(ursa::make_done_event());
        return ursa::Status::OK;
    };
    env.controller.submit("hello");
    REQUIRE(env.pump.wait_for([&] { return idle(env.controller.session()); }));
    CHECK(env.requests.size() == 2);
    CHECK(env.controller.session().error().empty());
    const auto& items = env.controller.session().items();
    bool found = false;
    for (const auto& it : items) {
        if (const auto* a = std::get_if<ursa::AssistantTurn>(&it)) {
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
    env.stream   = [ep, round](const ursa::ChatRequest& req,
                      const ursa::StreamCallback& cb) -> ursa::Status {
        ep->requests.push_back(req);
        cb(ursa::make_error_event(
            ursa::Status::BUDGET_EXCEEDED, "insufficient credits"));
        return ursa::Status::BUDGET_EXCEEDED;
    };
    env.controller.submit("hello");
    REQUIRE(env.pump.wait_for([&] { return idle(env.controller.session()); }));
    CHECK(env.requests.size() == 1);
    CHECK(env.controller.session().error()
        == "out of budget / insufficient credits: insufficient credits");
}

struct FakeApi {
    std::string response;
    std::string request;
    int port    = 0;
    int fd      = -1;
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
        request = acc;
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

    ursa::Route route;
    route.endpoint = "http://127.0.0.1:" + std::to_string(api.port)
        + "/chat/completions";
    route.api     = "http://127.0.0.1:" + std::to_string(api.port);
    route.api_key = "k";

    ursa::ChatRequest req;
    req.model = "gpt-4o";

    std::vector<ursa::StreamEvent> events;
    int retry_after = 0;
    const auto p    = ursa::get_provider(route);
    const ursa::Status st
        = ursa::stream(p, route, req,
            [&](const ursa::StreamEvent& ev) { events.push_back(ev); },
            &retry_after);

    CHECK(st == ursa::Status::RATE_LIMITED);
    CHECK(retry_after == 7);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == ursa::StreamEvent::Kind::ERROR);
    CHECK(events[0].error == ursa::Status::RATE_LIMITED);
    CHECK(events[0].text == "Rate limit exceeded");
}

TEST_CASE("stream emits CONNECTED then parses SSE on success")
{
    FakeApi api("HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "\r\n"
                "data: {\"choices\":[{\"delta\":{\"content\":\"Hi\"}}]}\n\n"
                "data: [DONE]\n\n");

    ursa::Route route;
    route.endpoint = "http://127.0.0.1:" + std::to_string(api.port)
        + "/chat/completions";
    route.api_key = "k";

    ursa::ChatRequest req;
    req.model = "gpt-4o";

    std::vector<ursa::StreamEvent> events;
    const auto p     = ursa::get_provider(route);
    const ursa::Status st = ursa::stream(p, route, req,
        [&](const ursa::StreamEvent& ev) { events.push_back(ev); });

    CHECK(st == ursa::Status::OK);
    REQUIRE(events.size() == 3);
    CHECK(events[0].kind == ursa::StreamEvent::Kind::CONNECTED);
    CHECK(events[1].kind == ursa::StreamEvent::Kind::CONTENT_DELTA);
    CHECK(events[1].text == "Hi");
    CHECK(events[2].kind == ursa::StreamEvent::Kind::DONE);
}

} // namespace
