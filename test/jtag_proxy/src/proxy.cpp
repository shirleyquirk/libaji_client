#include "jtag_proxy/proxy.hpp"

#include "decoder.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <format>
#include <fstream>
#include <iostream>
#include <list>
#include <mutex>
#include <system_error>
#include <thread>

namespace jtag_proxy {
namespace {

int connect_to(const std::string & host, std::uint16_t port)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo * res = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0)
        return -1;
    int fd = -1;
    for (addrinfo * ai = res; ai && fd < 0; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd >= 0 && ::connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            ::close(fd);
            fd = -1;
        }
    }
    ::freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return fd;
}

bool send_all(int fd, const std::uint8_t * p, std::size_t n)
{
    while (n > 0) {
        const ssize_t r = ::send(fd, p, n, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

} // namespace

struct Proxy::Impl {
    ProxyOptions options;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    std::atomic<std::uint64_t> seq = 0;

    mutable std::mutex mu; // logs and connection list
    std::vector<ScanRecord> scans;
    std::vector<std::string> wire;
    std::vector<std::string> errors;
    std::ofstream scan_file, wire_file;

    struct Connection {
        int client = -1, upstream = -1;
        std::jthread thread;
    };
    std::list<Connection> connections;
    unsigned sessions = 0;

    int listen_fd = -1;
    std::uint16_t port = 0;
    std::atomic<bool> stopping = false;
    std::jthread acceptor;

    explicit Impl(ProxyOptions o);
    ~Impl() { stop(); }
    void accept_loop();
    void serve(Connection & c, unsigned session);
    void stop();

    void on_scan(ScanRecord r)
    {
        std::lock_guard lk(mu);
        if (scan_file)
            scan_file << "- " << r.to_yaml() << '\n' << std::flush;
        if (options.echo)
            std::clog << r.to_text() << '\n';
        scans.push_back(std::move(r));
    }
    void on_wire(std::string line)
    {
        std::lock_guard lk(mu);
        if (wire_file)
            wire_file << line << '\n' << std::flush;
        wire.push_back(std::move(line));
    }
    void on_error(std::string line)
    {
        std::lock_guard lk(mu);
        if (wire_file)
            wire_file << "ERROR " << line << '\n' << std::flush;
        std::clog << "jtag-proxy: " << line << '\n';
        errors.push_back(std::move(line));
    }
};

Proxy::Impl::Impl(ProxyOptions o) : options(std::move(o))
{
    if (options.scan_log) {
        scan_file.open(*options.scan_log, std::ios::trunc);
        if (!scan_file)
            throw std::runtime_error(std::format("cannot write {}", options.scan_log->string()));
        scan_file << std::format("# jtag-proxy scan log; upstream {}:{}\n", options.upstream_host, options.upstream_port);
    }
    if (options.wire_log) {
        wire_file.open(*options.wire_log, std::ios::trunc);
        if (!wire_file)
            throw std::runtime_error(std::format("cannot write {}", options.wire_log->string()));
    }

    listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0)
        throw std::system_error(errno, std::generic_category(), "socket");
    int one = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(options.listen_port);
    if (::inet_pton(AF_INET, options.bind_address.c_str(), &addr.sin_addr) != 1)
        throw std::invalid_argument("bad bind address: " + options.bind_address);
    if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 || ::listen(listen_fd, 8) < 0) {
        const int err = errno;
        ::close(listen_fd);
        throw std::system_error(err, std::generic_category(), "bind/listen");
    }
    socklen_t len = sizeof(addr);
    ::getsockname(listen_fd, reinterpret_cast<sockaddr *>(&addr), &len);
    port = ntohs(addr.sin_port);
    acceptor = std::jthread([this] { accept_loop(); });
}

void Proxy::Impl::accept_loop()
{
    while (!stopping) {
        const int fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        const int up = connect_to(options.upstream_host, options.upstream_port);
        std::lock_guard lk(mu);
        if (up < 0 || stopping) {
            if (up < 0)
                errors.push_back(std::format("cannot connect to upstream {}:{}", options.upstream_host,
                                             options.upstream_port));
            ::close(fd);
            if (up >= 0)
                ::close(up);
            continue;
        }
        const unsigned session = ++sessions;
        Connection & c = connections.emplace_back();
        c.client = fd;
        c.upstream = up;
        c.thread = std::jthread([this, &c, session] { serve(c, session); });
    }
}

void Proxy::Impl::serve(Connection & c, unsigned session)
{
    const mock_jtagd::Scenario * names = options.names ? &*options.names : nullptr;
    Decoder decoder(session, names,
                    {[this](ScanRecord r) { on_scan(std::move(r)); }, [this](std::string l) { on_wire(std::move(l)); },
                     [this](std::string l) { on_error(std::move(l)); }},
                    t0, seq);
    std::mutex decoder_mu;

    // Forward one direction. The decoder sees each chunk before it is passed
    // on: that way a command is always decoded before the server can answer
    // it. The decoder swallows its own errors, so this can't block forwarding.
    auto pump = [&](int from, int to, bool client_side) {
        std::uint8_t buf[16384];
        for (;;) {
            const ssize_t n = ::recv(from, buf, sizeof(buf), 0);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            {
                std::lock_guard lk(decoder_mu);
                if (client_side)
                    decoder.from_client(std::span(buf, static_cast<std::size_t>(n)));
                else
                    decoder.from_server(std::span(buf, static_cast<std::size_t>(n)));
            }
            if (!send_all(to, buf, static_cast<std::size_t>(n)))
                break;
        }
        ::shutdown(from, SHUT_RDWR);
        ::shutdown(to, SHUT_RDWR);
    };
    {
        std::jthread up([&] { pump(c.upstream, c.client, false); });
        pump(c.client, c.upstream, true);
    }
    decoder.finish();
    on_wire(std::format("s{} closed", session));
}

void Proxy::Impl::stop()
{
    if (stopping.exchange(true))
        return;
    ::shutdown(listen_fd, SHUT_RDWR);
    if (acceptor.joinable())
        acceptor.join();
    ::close(listen_fd);
    std::list<Connection> conns;
    {
        std::lock_guard lk(mu);
        for (auto & c : connections) {
            ::shutdown(c.client, SHUT_RDWR);
            ::shutdown(c.upstream, SHUT_RDWR);
        }
        conns.splice(conns.end(), connections);
    }
    for (auto & c : conns) {
        if (c.thread.joinable())
            c.thread.join();
        ::close(c.client);
        ::close(c.upstream);
    }
}

Proxy::Proxy(ProxyOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
Proxy::~Proxy() = default;

std::uint16_t Proxy::port() const { return impl_->port; }
void Proxy::stop() { impl_->stop(); }

std::vector<ScanRecord> Proxy::scans() const
{
    std::lock_guard lk(impl_->mu);
    return impl_->scans;
}

std::vector<std::string> Proxy::wire_log() const
{
    std::lock_guard lk(impl_->mu);
    return impl_->wire;
}

std::vector<std::string> Proxy::decode_errors() const
{
    std::lock_guard lk(impl_->mu);
    return impl_->errors;
}

} // namespace jtag_proxy
