#pragma once

// Raw-socket loopback HTTP server core shared by the MCP and OAuth test
// fixtures: exactly one request per connection, each answered with
// "Connection: close". Fixtures install a Handler before start(); knobs
// and scripted responses belong to them, transport plumbing lives here.

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <string>
#include <thread>

// winsock2.h must precede windows.h, which other headers may pull in.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "test_platform.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace imza::test {

namespace detail {

// Thin socket shim so the fixture body below reads the same on every
// platform. Windows spells the same operations differently and needs a
// process-wide WSAStartup before the first call.
#ifdef _WIN32
    using Socket                          = SOCKET;
    constexpr Socket INVALID_SOCKET_VALUE = INVALID_SOCKET;

    inline bool socket_layer_ready()
    {
        static const bool ready = [] {
            WSADATA data { };
            return WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        return ready;
    }

    inline Socket open_socket()
    {
        return socket_layer_ready() ? ::socket(AF_INET, SOCK_STREAM, 0)
                                    : INVALID_SOCKET;
    }
    inline void close_socket(Socket handle)
    {
        if (handle != INVALID_SOCKET) {
            ::closesocket(handle);
        }
    }
    inline int send_bytes(Socket handle, const char* data, std::size_t size)
    {
        return ::send(handle, data, static_cast<int>(size), 0);
    }
    inline int receive_bytes(Socket handle, char* data, std::size_t size)
    {
        return ::recv(handle, data, static_cast<int>(size), 0);
    }
    inline bool set_receive_timeout(Socket handle, int seconds)
    {
        const DWORD timeout = static_cast<DWORD>(seconds) * 1000;
        return ::setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout))
            == 0;
    }
    inline bool set_send_timeout(Socket handle, int seconds)
    {
        const DWORD timeout = static_cast<DWORD>(seconds) * 1000;
        return ::setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout))
            == 0;
    }
    inline bool set_reuse_address(Socket handle)
    {
        const BOOL enabled = TRUE;
        return ::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&enabled), sizeof(enabled))
            == 0;
    }
    inline Socket accept_socket(Socket listener)
    {
        return ::accept(listener, nullptr, nullptr);
    }
    // shutdown() does not wake accept() on a listening Winsock socket, so
    // the server polls instead of blocking. >0 ready, 0 timeout, -1 failed.
    inline int wait_readable(Socket handle, int timeout_ms)
    {
        WSAPOLLFD entry { handle, POLLRDNORM, 0 };
        return ::WSAPoll(&entry, 1, timeout_ms);
    }
    inline bool bind_loopback(Socket handle, sockaddr_in& address)
    {
        return ::bind(handle, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address))
            == 0;
    }
    inline bool listen_socket(Socket handle)
    {
        return ::listen(handle, 8) == 0;
    }
    inline bool bound_port(Socket handle, sockaddr_in& address)
    {
        int length = sizeof(address);
        return ::getsockname(
                   handle, reinterpret_cast<sockaddr*>(&address), &length)
            == 0;
    }
#else
    using Socket                          = int;
    constexpr Socket INVALID_SOCKET_VALUE = -1;

    inline bool socket_layer_ready() { return true; }
    inline Socket open_socket() { return ::socket(AF_INET, SOCK_STREAM, 0); }
    inline void close_socket(Socket handle)
    {
        if (handle >= 0) {
            ::close(handle);
        }
    }
    inline int send_bytes(Socket handle, const char* data, std::size_t size)
    {
        return static_cast<int>(::send(handle, data, size, 0));
    }
    inline int receive_bytes(Socket handle, char* data, std::size_t size)
    {
        return static_cast<int>(::recv(handle, data, size, 0));
    }
    inline bool set_receive_timeout(Socket handle, int seconds)
    {
        const timeval timeout { seconds, 0 };
        return ::setsockopt(
                   handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout))
            == 0;
    }
    inline bool set_send_timeout(Socket handle, int seconds)
    {
        const timeval timeout { seconds, 0 };
        return ::setsockopt(
                   handle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))
            == 0;
    }
    inline bool set_reuse_address(Socket handle)
    {
        int enabled = 1;
        return ::setsockopt(
                   handle, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled))
            == 0;
    }
    inline Socket accept_socket(Socket listener)
    {
        return ::accept(listener, nullptr, nullptr);
    }
    // >0 ready, 0 timeout, -1 failed. EINTR reads as a timeout so the caller
    // keeps one shape on every platform.
    inline int wait_readable(Socket handle, int timeout_ms)
    {
        pollfd entry { handle, POLLIN, 0 };
        const int ready = ::poll(&entry, 1, timeout_ms);
        return ready < 0 && errno == EINTR ? 0 : ready;
    }
    inline bool bind_loopback(Socket handle, sockaddr_in& address)
    {
        return ::bind(handle, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address))
            == 0;
    }
    inline bool listen_socket(Socket handle)
    {
        return ::listen(handle, 8) == 0;
    }
    inline bool bound_port(Socket handle, sockaddr_in& address)
    {
        socklen_t length = sizeof(address);
        return ::getsockname(
                   handle, reinterpret_cast<sockaddr*>(&address), &length)
            == 0;
    }
#endif

} // namespace detail

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
    using Handler = std::function<void(detail::Socket, const LoopbackRequest&)>;

    // Set before start(); answers each accepted connection and may call
    // respond(). Runs on the server thread.
    Handler handle;

    ~LoopbackHttpServer() { stop(); }

    LoopbackHttpServer()                                     = default;
    LoopbackHttpServer(const LoopbackHttpServer&)            = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

    bool start()
    {
        listen_fd_ = detail::open_socket();
        if (listen_fd_ == detail::INVALID_SOCKET_VALUE) {
            return false;
        }
        detail::set_reuse_address(listen_fd_);
        sockaddr_in address { };
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port        = 0;
        if (!detail::bind_loopback(listen_fd_, address)
            || !detail::listen_socket(listen_fd_)) {
            detail::close_socket(listen_fd_);
            listen_fd_ = detail::INVALID_SOCKET_VALUE;
            return false;
        }
        sockaddr_in bound { };
        if (!detail::bound_port(listen_fd_, bound)) {
            detail::close_socket(listen_fd_);
            listen_fd_ = detail::INVALID_SOCKET_VALUE;
            return false;
        }
        base_ = "http://127.0.0.1:" + std::to_string(ntohs(bound.sin_port));
        stopping_.store(false);
        thread_ = std::thread([this] { serve(); });
        return true;
    }

    void stop()
    {
        stopping_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listen_fd_ != detail::INVALID_SOCKET_VALUE) {
            detail::close_socket(listen_fd_);
            listen_fd_ = detail::INVALID_SOCKET_VALUE;
        }
    }

    const std::string& base_url() const { return base_; }

    static void respond(detail::Socket fd, const std::string& status,
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
            const int sent_now = detail::send_bytes(
                fd, wire.data() + sent, wire.size() - sent);
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
            if (stopping_.load()) {
                return;
            }
            const int readable = detail::wait_readable(listen_fd_, 50);
            if (readable == 0) {
                continue;
            }
            if (readable < 0) {
                return;
            }
            const detail::Socket fd = detail::accept_socket(listen_fd_);
            if (fd == detail::INVALID_SOCKET_VALUE) {
                return;
            }
            detail::set_receive_timeout(fd, 5);
            // respond() streams the scripted answer back; without a send
            // timeout a stalled peer would park the server thread forever
            // and a stop() join would never return.
            detail::set_send_timeout(fd, 5);
            LoopbackRequest request;
            if (read_request(fd, request) && handle) {
                handle(fd, request);
            }
            detail::close_socket(fd);
        }
    }

    static bool read_request(detail::Socket fd, LoopbackRequest& request)
    {
        std::string buffer;
        char chunk[4096];
        std::size_t header_end = std::string::npos;
        while ((header_end = buffer.find("\r\n\r\n")) == std::string::npos) {
            const int received
                = detail::receive_bytes(fd, chunk, sizeof(chunk));
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
            const int received
                = detail::receive_bytes(fd, chunk, sizeof(chunk));
            if (received <= 0) {
                return false;
            }
            request.body.append(chunk, static_cast<std::size_t>(received));
        }
        return true;
    }

    detail::Socket listen_fd_ = detail::INVALID_SOCKET_VALUE;
    std::atomic<bool> stopping_ { false };
    std::thread thread_;
    std::string base_;
};

inline void allow_loopback_direct()
{
    // Bypass any ambient proxy settings for the local fixture server.
    set_env("NO_PROXY", "127.0.0.1,localhost");
    set_env("no_proxy", "127.0.0.1,localhost");
}

} // namespace imza::test
