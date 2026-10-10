#pragma once

// Raw-socket loopback HTTP server core shared by the MCP and OAuth test
// fixtures: exactly one request per connection, each answered with
// "Connection: close". Fixtures install a Handler before start(); knobs
// and scripted responses belong to them, transport plumbing lives here.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <string>
#include <thread>

namespace imza::test {

struct LoopbackRequest {
    std::string method;
    std::string path;  // without the query string
    std::string query; // without the leading '?'
    std::string headers;
    std::string lower_headers;
    std::string body;
};

class LoopbackHttpServer {
public:
    using Handler = std::function<void(int fd, const LoopbackRequest&)>;

    // Set before start(); answers each accepted connection and may call
    // respond(). Runs on the server thread.
    Handler handle;

    ~LoopbackHttpServer() { stop(); }

    LoopbackHttpServer()                                     = default;
    LoopbackHttpServer(const LoopbackHttpServer&)            = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

    bool start()
    {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return false;
        }
        int reuse = 1;
        ::setsockopt(
            listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address { };
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port        = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address),
                sizeof(address))
                < 0
            || ::listen(listen_fd_, 8) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        sockaddr_in bound { };
        socklen_t length = sizeof(bound);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &length);
        base_   = "http://127.0.0.1:" + std::to_string(ntohs(bound.sin_port));
        thread_ = std::thread([this] { serve(); });
        return true;
    }

    void stop()
    {
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    const std::string& base_url() const { return base_; }

    static void respond(int fd, const std::string& status,
        std::initializer_list<std::string> headers, const std::string& body)
    {
        std::string wire = status + "\r\nConnection: close\r\n";
        for (const std::string& header : headers) {
            wire += header + "\r\n";
        }
        wire += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        wire += body;
        std::size_t sent = 0;
        while (sent < wire.size()) {
            const ssize_t sent_now
                = ::send(fd, wire.data() + sent, wire.size() - sent, 0);
            if (sent_now <= 0) {
                return;
            }
            sent += static_cast<std::size_t>(sent_now);
        }
    }

private:
    void serve()
    {
        for (;;) {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                return; // unblocked by stop()'s shutdown
            }
            const timeval timeout { 5, 0 };
            ::setsockopt(
                fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            LoopbackRequest request;
            if (read_request(fd, request) && handle) {
                handle(fd, request);
            }
            ::close(fd);
        }
    }

    static bool read_request(int fd, LoopbackRequest& request)
    {
        std::string buffer;
        char chunk[4096];
        std::size_t header_end = std::string::npos;
        while ((header_end = buffer.find("\r\n\r\n")) == std::string::npos) {
            const ssize_t received = ::recv(fd, chunk, sizeof(chunk), 0);
            if (received <= 0) {
                return false;
            }
            buffer.append(chunk, static_cast<std::size_t>(received));
            if (buffer.size() > (1u << 20)) {
                return false;
            }
        }
        const std::size_t line_end = buffer.find("\r\n");
        const std::size_t space    = buffer.find(' ');
        if (line_end == std::string::npos || space == std::string::npos
            || space > line_end) {
            return false;
        }
        const std::size_t second = buffer.find(' ', space + 1);
        if (second == std::string::npos || second > line_end) {
            return false;
        }
        request.method           = buffer.substr(0, space);
        const std::string target = buffer.substr(space + 1, second - space - 1);
        const auto query_at      = target.find('?');
        request.path             = target.substr(0, query_at);
        request.query
            = query_at == std::string::npos ? "" : target.substr(query_at + 1);
        request.headers = buffer.substr(0, header_end);
        for (const char c : request.headers) {
            request.lower_headers += static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
        }
        std::size_t content_length = 0;
        if (const std::size_t at
            = request.lower_headers.find("content-length:");
            at != std::string::npos) {
            content_length = std::strtoul(
                request.lower_headers.c_str() + at + 15, nullptr, 10);
        }
        request.body = buffer.substr(header_end + 4);
        while (request.body.size() < content_length) {
            const ssize_t received = ::recv(fd, chunk, sizeof(chunk), 0);
            if (received <= 0) {
                return false;
            }
            request.body.append(chunk, static_cast<std::size_t>(received));
        }
        return true;
    }

    int listen_fd_ = -1;
    std::thread thread_;
    std::string base_;
};

inline void allow_loopback_direct()
{
    // Bypass any ambient proxy settings for the local fixture server.
    ::setenv("NO_PROXY", "127.0.0.1,localhost", 1);
    ::setenv("no_proxy", "127.0.0.1,localhost", 1);
}

} // namespace imza::test
