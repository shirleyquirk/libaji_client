#include "mock_jtagd/server.hpp"

#include "bits.hpp"
#include "chain_model.hpp"
#include "md5.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <system_error>
#include <thread>

namespace mock_jtagd {
namespace {

// Open ids live above every chain id so UNLOCK_LOCK_DEVICE can tell them apart.
constexpr std::uint32_t open_id_base = 0x10000;
constexpr std::uint8_t authtype_md5 = static_cast<std::uint8_t>(Cmd::authenticate_md5);

struct Disconnect {};

std::size_t bytes_for(std::uint32_t bits) { return (std::size_t{bits} + 7) / 8; }

struct Claim {
    std::uint32_t type;
    std::uint32_t length;
    std::uint64_t value;
};

// Two claims on the same TAP conflict when they name the same IR (or overlay)
// value and at least one wants it exclusively. Weak claims never conflict.
// [inferred: real jtagd's exact rules aren't visible from the client]
bool claims_conflict(const Claim & a, const Claim & b)
{
    if ((a.type & claim::overlay_bit) != (b.type & claim::overlay_bit) || a.value != b.value)
        return false;
    if ((a.type & claim::weak_bit) || (b.type & claim::weak_bit))
        return false;
    return !(a.type & claim::shared_bit) || !(b.type & claim::shared_bit);
}

struct Owner {
    unsigned session = 0;      // 0 = unlocked
    std::uint32_t open_id = 0; // 0 = locked at chain level
};

struct ChainRec {
    std::size_t index;         // position in Impl::chains; chain id = index + 1
    Cable cable;
    ChainModel model;
    std::uint32_t tag = 0;     // 0 = never scanned
    bool removed = false;
    Owner owner;
};

struct OpenRec {
    std::uint32_t id;
    unsigned session;
    std::size_t chain;
    std::uint32_t tap;
    bool whole_chain;
    std::vector<Claim> claims;
    std::string app;
};

struct DefinedDevice {
    std::uint32_t idcode, ir_length, features, mask;
    std::string name;
};

} // namespace

struct Server::Impl {
    Scenario scenario;
    ServerOptions options;

    mutable std::mutex mu;
    std::condition_variable cv;
    std::deque<ChainRec> chains; // deque: references survive ADD_HARDWARE during a lock wait
    std::map<std::uint32_t, OpenRec> opens;
    std::uint32_t next_open_id = open_id_base + 1;
    std::uint32_t next_tag = 1;
    std::vector<DefinedDevice> defined;
    std::uint32_t defined_tag = 1;
    std::vector<Fault> faults;
    std::vector<CommandRecord> records;
    std::map<unsigned, int> session_fds;
    unsigned session_count = 0;

    int listen_fd = -1;
    std::uint16_t bound_port = 0;
    std::atomic<bool> stopping = false;
    std::jthread acceptor;
    std::vector<std::jthread> sessions;

    Impl(Scenario s, ServerOptions o);
    ~Impl() { stop(); }
    void accept_loop();
    void stop();

    // Must hold mu.
    ChainRec * find_chain(std::uint32_t chain_id)
    {
        if (chain_id == 0 || chain_id > chains.size() || chains[chain_id - 1].removed)
            return nullptr;
        return &chains[chain_id - 1];
    }
    OpenRec * find_open(std::uint32_t open_id, unsigned session)
    {
        auto it = opens.find(open_id);
        return (it == opens.end() || it->second.session != session) ? nullptr : &it->second;
    }
    std::optional<Fault> take_fault(Cmd cmd)
    {
        for (auto it = faults.begin(); it != faults.end(); ++it) {
            if (it->cmd != cmd)
                continue;
            if (it->skip > 0) {
                --it->skip;
                return std::nullopt;
            }
            Fault f = *it;
            faults.erase(it);
            return f;
        }
        return std::nullopt;
    }
    std::string app_name(const Owner & o) const
    {
        if (o.open_id != 0)
            if (auto it = opens.find(o.open_id); it != opens.end())
                return it->second.app;
        return std::format("mock-session-{}", o.session);
    }
};

namespace {

class Session {
public:
    Session(Server::Impl & srv, int fd, unsigned id) : srv_(srv), fd_(fd), id_(id) {}
    void run();

private:
    struct Ctx {
        Reader & in;
        Writer & out;
        bool exec;
    };

    // Transport
    void read_exact(std::uint8_t * p, std::size_t n);
    void send_all(const std::uint8_t * p, std::size_t n);
    std::pair<unsigned, std::vector<std::uint8_t>> read_packet();
    void send_packet(unsigned mux, std::span<const std::uint8_t> payload);
    std::vector<std::uint8_t> next_command_packet();
    std::vector<std::uint8_t> fifo_read(unsigned index, std::size_t n);
    void fifo_write(unsigned index, std::span<const std::uint8_t> data);

    // Protocol
    bool handshake();
    void handle_packet(std::span<const std::uint8_t> payload);
    Err dispatch(Cmd cmd, Ctx & ctx);
    void cleanup();
    void trace(std::string_view msg) const
    {
        if (srv_.options.trace)
            std::clog << std::format("[mock-jtagd s{}] {}\n", id_, msg);
    }

    // Locking helpers; caller holds srv_.mu via `lk`.
    Err acquire(std::unique_lock<std::mutex> & lk, ChainRec & chain, std::uint32_t open_id,
                std::uint32_t timeout_ms, Writer & out);
    void write_blocker(Writer & out, const Owner & owner) const;
    bool owns_chain(const ChainRec & chain) const { return chain.owner.session == id_; }
    Err check_device_lock(const OpenRec * open) const;

    // Command handlers
    Err cmd_get_hardware(Ctx & c);
    Err cmd_add_hardware(Ctx & c);
    Err cmd_remove_hardware(Ctx & c);
    Err cmd_get_version_info(Ctx & c);
    Err cmd_use_protocol_version(Ctx & c);
    Err cmd_lock_chain(Ctx & c);
    Err cmd_unlock_chain(Ctx & c);
    Err cmd_read_chain(Ctx & c, bool force_scan);
    Err cmd_define_device(Ctx & c);
    Err cmd_get_defined_devices(Ctx & c);
    Err cmd_set_parameter(Ctx & c);
    Err cmd_get_parameter(Ctx & c);
    Err cmd_open_device(Ctx & c);
    Err cmd_open_entire_chain(Ctx & c);
    Err cmd_close_device(Ctx & c);
    Err cmd_lock_device(Ctx & c);
    Err cmd_unlock_device(Ctx & c);
    Err cmd_unlock_lock_device(Ctx & c);
    Err cmd_access_ir(Ctx & c);
    Err cmd_access_ir_fifo(Ctx & c);
    Err cmd_access_dr(Ctx & c);
    Err cmd_any_sequence(Ctx & c);
    Err cmd_simple_device_op(Ctx & c, Cmd cmd);
    Err cmd_check_password(Ctx & c);

    Server::Impl & srv_;
    int fd_;
    unsigned id_;
    std::uint32_t version_ = 0;
    unsigned packet_no_ = 0;
    bool last_packet_failed_ = false;
    Err last_error_ = Err::no_error;
    std::array<std::deque<std::uint8_t>, mux_count - fifo_mux_base> fifo_in_;
    std::deque<std::vector<std::uint8_t>> pending_;
    std::array<std::uint8_t, 16> challenge_{};
};

// ---------------------------------------------------------------- transport

void Session::read_exact(std::uint8_t * p, std::size_t n)
{
    while (n > 0) {
        const ssize_t r = ::recv(fd_, p, n, 0);
        if (r > 0) {
            p += r;
            n -= static_cast<std::size_t>(r);
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else {
            throw Disconnect{};
        }
    }
}

void Session::send_all(const std::uint8_t * p, std::size_t n)
{
    while (n > 0) {
        const ssize_t r = ::send(fd_, p, n, MSG_NOSIGNAL);
        if (r > 0) {
            p += r;
            n -= static_cast<std::size_t>(r);
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else {
            throw Disconnect{};
        }
    }
}

std::pair<unsigned, std::vector<std::uint8_t>> Session::read_packet()
{
    std::uint8_t hdr[2];
    read_exact(hdr, 2);
    const unsigned header = (unsigned{hdr[0]} << 8) | hdr[1];
    std::vector<std::uint8_t> payload((header & 0x0FFF) + 1);
    read_exact(payload.data(), payload.size());
    return {header >> 12, std::move(payload)};
}

void Session::send_packet(unsigned mux, std::span<const std::uint8_t> payload)
{
    if (payload.empty() || payload.size() > max_payload)
        throw std::logic_error(std::format("packet payload size {} out of range", payload.size()));
    const unsigned header = (mux << 12) | static_cast<unsigned>(payload.size() - 1);
    std::vector<std::uint8_t> buf{static_cast<std::uint8_t>(header >> 8), static_cast<std::uint8_t>(header)};
    buf.insert(buf.end(), payload.begin(), payload.end());
    send_all(buf.data(), buf.size());
}

std::vector<std::uint8_t> Session::next_command_packet()
{
    if (!pending_.empty()) {
        auto p = std::move(pending_.front());
        pending_.pop_front();
        return p;
    }
    for (;;) {
        auto [mux, payload] = read_packet();
        if (mux == mux_command)
            return std::move(payload);
        if (mux >= fifo_mux_base)
            fifo_in_[mux - fifo_mux_base].insert(fifo_in_[mux - fifo_mux_base].end(), payload.begin(), payload.end());
    }
}

std::vector<std::uint8_t> Session::fifo_read(unsigned index, std::size_t n)
{
    auto & q = fifo_in_.at(index);
    while (q.size() < n) {
        auto [mux, payload] = read_packet();
        if (mux == mux_command)
            pending_.push_back(std::move(payload)); // pipelined (CONTINUE_COMMANDS) packet
        else if (mux >= fifo_mux_base)
            fifo_in_[mux - fifo_mux_base].insert(fifo_in_[mux - fifo_mux_base].end(), payload.begin(), payload.end());
    }
    std::vector<std::uint8_t> out(q.begin(), q.begin() + static_cast<std::ptrdiff_t>(n));
    q.erase(q.begin(), q.begin() + static_cast<std::ptrdiff_t>(n));
    return out;
}

void Session::fifo_write(unsigned index, std::span<const std::uint8_t> data)
{
    for (std::size_t off = 0; off < data.size(); off += max_payload)
        send_packet(fifo_mux_base + index, data.subspan(off, std::min(max_payload, data.size() - off)));
}

// ---------------------------------------------------------------- protocol

void Session::run()
{
    try {
        if (handshake())
            for (;;)
                handle_packet(next_command_packet());
    } catch (const Disconnect &) {
        trace("client disconnected");
    } catch (const std::exception & e) {
        trace(std::format("dropping connection: {}", e.what()));
    }
    cleanup();
}

bool Session::handshake()
{
    const Scenario & sc = srv_.scenario;
    Writer greet;
    greet.str(signature);
    greet.u32(sc.version);
    if (sc.password) {
        greet.u32(authtype_md5);
        std::random_device rd;
        for (auto & b : challenge_)
            b = static_cast<std::uint8_t>(rd());
        greet.raw(challenge_);
    } else {
        greet.u32(0);
    }
    send_packet(mux_command, greet.bytes());
    version_ = sc.version; // until the client says otherwise

    if (!sc.password)
        return true;

    const auto payload = next_command_packet();
    const auto blocks = split_blocks(payload);
    bool ok = false;
    if (blocks.size() == 1 && blocks[0].code == authtype_md5 && blocks[0].body.size() >= 16) {
        std::vector<std::uint8_t> msg(challenge_.begin(), challenge_.end());
        msg.insert(msg.end(), sc.password->begin(), sc.password->end());
        const auto expect = md5(msg);
        ok = std::equal(expect.begin(), expect.end(), blocks[0].body.begin());
    }
    Writer resp;
    resp.begin_block(static_cast<std::uint8_t>(ok ? Err::no_error : Err::not_permitted));
    resp.end_block();
    send_packet(mux_command, resp.bytes());
    {
        std::lock_guard lk(srv_.mu);
        srv_.records.push_back({id_, ++packet_no_, Cmd::authenticate_md5,
                                ok ? Err::no_error : Err::not_permitted, true});
    }
    trace(ok ? "MD5 authentication ok" : "MD5 authentication FAILED");
    return ok; // [inferred] real server drops the link after a bad password
}

void Session::handle_packet(std::span<const std::uint8_t> payload)
{
    const auto blocks = split_blocks(payload);
    ++packet_no_;
    Writer resp;
    bool executing = true;

    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const Cmd cmd = static_cast<Cmd>(blocks[i].code);
        Reader in(blocks[i].body);
        Writer body;
        Err err = Err::no_error;
        bool executed = false;

        std::optional<Fault> fault;
        if (executing) {
            std::lock_guard lk(srv_.mu);
            fault = srv_.take_fault(cmd);
        }
        if (fault && fault->disconnect) {
            trace(std::format("{}: injected disconnect", to_string(cmd)));
            throw Disconnect{};
        }

        if (cmd == Cmd::continue_commands && i == 0 && last_packet_failed_) {
            // [inferred] a continuation of a failed batch fails with the same error.
            err = last_error_;
            executed = true;
        } else if (executing && !fault) {
            Ctx ctx{in, body, true};
            err = dispatch(cmd, ctx);
            executed = true;
        } else {
            // Not executed, but its FIFO payload still has to be drained.
            Ctx ctx{in, body, false};
            dispatch(cmd, ctx);
            body.clear();
            if (fault) {
                err = fault->error;
                executed = true;
                trace(std::format("{}: injected error {}", to_string(cmd), static_cast<int>(err)));
            }
        }

        if (executed) {
            resp.begin_block(0);
            resp.raw(body.bytes());
            resp.end_block(static_cast<std::uint8_t>(err));
            if (err != Err::no_error) {
                executing = false;
                last_error_ = err;
            }
        }
        {
            std::lock_guard lk(srv_.mu);
            srv_.records.push_back({id_, packet_no_, cmd, executed ? err : Err::no_error, executed});
        }
        trace(std::format("p{} {} -> {}{}", packet_no_, to_string(cmd), static_cast<int>(err),
                          executed ? "" : " (skipped)"));
    }
    last_packet_failed_ = !executing;
    send_packet(mux_command, resp.bytes());
}

Err Session::dispatch(Cmd cmd, Ctx & c)
{
    switch (cmd) {
    case Cmd::get_hardware: return cmd_get_hardware(c);
    case Cmd::add_hardware: return cmd_add_hardware(c);
    case Cmd::remove_hardware: return cmd_remove_hardware(c);
    case Cmd::get_version_info: return cmd_get_version_info(c);
    case Cmd::use_protocol_version: return cmd_use_protocol_version(c);
    case Cmd::lock_chain: return cmd_lock_chain(c);
    case Cmd::unlock_chain: return cmd_unlock_chain(c);
    case Cmd::scan_chain: return cmd_read_chain(c, true);
    case Cmd::read_chain: return cmd_read_chain(c, false);
    case Cmd::define_device: return cmd_define_device(c);
    case Cmd::get_defined_devices: return cmd_get_defined_devices(c);
    case Cmd::get_quartus_devices:
        if (c.exec) {
            c.out.u32(0);
            c.out.u32(0);
        }
        return Err::no_error;
    case Cmd::set_parameter: return cmd_set_parameter(c);
    case Cmd::get_parameter: return cmd_get_parameter(c);
    case Cmd::open_device: return cmd_open_device(c);
    case Cmd::open_entire_chain: return cmd_open_entire_chain(c);
    case Cmd::close_device: return cmd_close_device(c);
    case Cmd::lock_device: return cmd_lock_device(c);
    case Cmd::unlock_device: return cmd_unlock_device(c);
    case Cmd::unlock_lock_device: return cmd_unlock_lock_device(c);
    case Cmd::access_ir: return cmd_access_ir(c);
    case Cmd::access_ir_fifo: return cmd_access_ir_fifo(c);
    case Cmd::access_dr: return cmd_access_dr(c);
    case Cmd::any_sequence: return cmd_any_sequence(c);
    case Cmd::run_test_idle:
    case Cmd::test_logic_reset:
    case Cmd::delay_microseconds: return cmd_simple_device_op(c, cmd);
    case Cmd::check_password: return cmd_check_password(c);
    case Cmd::ping:
    case Cmd::continue_commands: return Err::no_error;
    default: return Err::unimplemented; // GET_PARAMETER_BLOCK, WATCH_DATA, ...
    }
}

void Session::cleanup()
{
    {
        std::lock_guard lk(srv_.mu);
        for (auto & ch : srv_.chains)
            if (ch.owner.session == id_)
                ch.owner = {};
        std::erase_if(srv_.opens, [&](const auto & kv) { return kv.second.session == id_; });
        srv_.session_fds.erase(id_);
    }
    srv_.cv.notify_all();
    ::close(fd_);
}

// ---------------------------------------------------------------- locking

void Session::write_blocker(Writer & out, const Owner & owner) const
{
    out.u32(1);
    out.str(srv_.app_name(owner));
    out.u32((4u << 16) | 4u);
    const std::uint8_t loopback[4] = {127, 0, 0, 1};
    out.raw(loopback);
}

Err Session::acquire(std::unique_lock<std::mutex> & lk, ChainRec & chain, std::uint32_t open_id,
                     std::uint32_t timeout_ms, Writer & out)
{
    if (chain.owner.session == id_)
        return Err::locked;
    const bool got = srv_.cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                      [&] { return chain.owner.session == 0 || chain.removed; });
    if (chain.removed)
        return Err::invalid_chain_id;
    if (!got) {
        write_blocker(out, chain.owner);
        return Err::chain_in_use;
    }
    chain.owner = {id_, open_id};
    return Err::no_error;
}

Err Session::check_device_lock(const OpenRec * open) const
{
    if (open == nullptr)
        return Err::invalid_open_id;
    const ChainRec & chain = srv_.chains[open->chain];
    if (chain.owner.session != id_ || chain.owner.open_id != open->id)
        return Err::not_locked;
    return Err::no_error;
}

// ---------------------------------------------------------------- hardware

Err Session::cmd_get_hardware(Ctx & c)
{
    if (version_ >= 1 && !c.in.at_end())
        c.in.u32(); // client's protocol version, informational
    if (!c.exec)
        return Err::no_error;

    Writer list;
    std::uint32_t n = 0;
    {
        std::lock_guard lk(srv_.mu);
        for (std::size_t i = 0; i < srv_.chains.size(); ++i) {
            const ChainRec & ch = srv_.chains[i];
            if (ch.removed)
                continue;
            ++n;
            list.u32(Server::chain_id(i));
            list.str(ch.cable.hw_name);
            list.str(ch.cable.port);
            list.u32(ch.cable.chain_type);
            list.str(ch.cable.device_name);
            if (version_ >= 2)
                list.u32(ch.cable.features);
        }
    }
    if (version_ < 1)
        return Err::unimplemented; // v0 inline hardware list not supported by the mock
    if (!list.empty())
        fifo_write(0, list.bytes());
    c.out.u32(n);
    c.out.u32(static_cast<std::uint32_t>(list.size()));
    return Err::no_error;
}

Err Session::cmd_add_hardware(Ctx & c)
{
    c.in.u32(); // hw_type, only meaningful to v0 servers
    Cable cable;
    cable.port = c.in.str();
    if (version_ >= 1) {
        cable.hw_name = c.in.str();
        cable.device_name = c.in.str();
    }
    if (!c.exec)
        return Err::no_error;
    std::lock_guard lk(srv_.mu);
    srv_.chains.push_back({srv_.chains.size(), cable, ChainModel({}), 0, false, {}});
    return Err::no_error;
}

Err Session::cmd_remove_hardware(Ctx & c)
{
    const std::uint32_t chain_id = c.in.u32();
    if (!c.exec)
        return Err::no_error;
    {
        std::lock_guard lk(srv_.mu);
        ChainRec * ch = srv_.find_chain(chain_id);
        if (ch == nullptr)
            return Err::invalid_chain_id;
        if (ch->owner.session != 0 && ch->owner.session != id_)
            return Err::chain_in_use;
        ch->removed = true;
        ch->owner = {};
    }
    srv_.cv.notify_all();
    return Err::no_error;
}

Err Session::cmd_get_version_info(Ctx & c)
{
    if (!c.exec)
        return Err::no_error;
    c.out.str(srv_.scenario.version_info);
    c.out.u32(0); // pgmparts version; client reads and discards it
    c.out.str(srv_.scenario.server_path);
    return Err::no_error;
}

Err Session::cmd_use_protocol_version(Ctx & c)
{
    const std::uint32_t v = c.in.u32();
    if (!c.exec)
        return Err::no_error;
    version_ = std::min(v, srv_.scenario.version);
    c.out.u32(srv_.scenario.server_flags);
    return Err::no_error;
}

Err Session::cmd_check_password(Ctx & c)
{
    const std::uint32_t authtype = c.in.u32();
    const auto challenge = c.in.raw(16);
    const auto response = c.in.raw(16);
    if (!c.exec)
        return Err::no_error;
    if (authtype != authtype_md5)
        return Err::invalid_parameter;
    if (!srv_.scenario.password)
        return Err::not_permitted;
    std::vector<std::uint8_t> msg = challenge;
    msg.insert(msg.end(), srv_.scenario.password->begin(), srv_.scenario.password->end());
    const auto expect = md5(msg);
    return std::equal(expect.begin(), expect.end(), response.begin()) ? Err::no_error : Err::failure;
}

// ---------------------------------------------------------------- chain level

Err Session::cmd_lock_chain(Ctx & c)
{
    const std::uint32_t chain_id = c.in.u32();
    const std::uint32_t timeout = c.in.u32();
    if (!c.exec)
        return Err::no_error;
    std::unique_lock lk(srv_.mu);
    ChainRec * ch = srv_.find_chain(chain_id);
    if (ch == nullptr)
        return Err::invalid_chain_id;
    return acquire(lk, *ch, 0, timeout, c.out);
}

Err Session::cmd_unlock_chain(Ctx & c)
{
    const std::uint32_t chain_id = c.in.u32();
    if (!c.exec)
        return Err::no_error;
    {
        std::lock_guard lk(srv_.mu);
        ChainRec * ch = srv_.find_chain(chain_id);
        if (ch == nullptr)
            return Err::invalid_chain_id;
        if (ch->owner.session != id_ || ch->owner.open_id != 0)
            return Err::not_locked;
        ch->owner = {};
    }
    srv_.cv.notify_all();
    return Err::no_error;
}

Err Session::cmd_read_chain(Ctx & c, bool force_scan)
{
    const std::uint32_t chain_id = c.in.u32();
    const std::uint32_t known_tag = c.in.u32();
    const bool autoscan = version_ >= 2 && c.in.u32() != 0;
    if (!c.exec)
        return Err::no_error;

    Writer list;
    std::uint32_t tag = 0, n = 0;
    {
        std::lock_guard lk(srv_.mu);
        ChainRec * ch = srv_.find_chain(chain_id);
        if (ch == nullptr)
            return Err::invalid_chain_id;
        if (!owns_chain(*ch))
            return Err::not_locked;
        if (force_scan || (ch->tag == 0 && autoscan)) {
            ch->tag = srv_.next_tag++;
            ch->model.test_logic_reset();
        }
        if (ch->tag == 0)
            return Err::chain_not_configured;
        tag = ch->tag;
        const auto & taps = ch->model.taps();
        n = static_cast<std::uint32_t>(taps.size());
        if (known_tag != tag || n == 0)
            for (const Tap & t : taps) {
                list.u32(t.idcode);
                list.u32(t.ir_length);
                list.u32(t.features);
                list.u32(0);
                list.u32(0);
                list.str(t.name);
            }
    }
    if (!list.empty())
        fifo_write(0, list.bytes());
    c.out.u32(tag);
    c.out.u32(n);
    c.out.u32(static_cast<std::uint32_t>(list.size()));
    return Err::no_error;
}

Err Session::cmd_define_device(Ctx & c)
{
    const std::uint32_t chain_id = c.in.u32();
    const std::uint32_t tap_pos = c.in.u32();
    DefinedDevice dev;
    dev.idcode = c.in.u32();
    dev.ir_length = c.in.u32();
    dev.features = c.in.u32();
    dev.mask = c.in.u32();
    c.in.u32();
    dev.name = c.in.str();
    if (!c.exec)
        return Err::no_error;

    std::lock_guard lk(srv_.mu);
    if (chain_id == 0) {
        std::erase_if(srv_.defined, [&](const DefinedDevice & d) {
            return dev.idcode != 0 ? d.idcode == dev.idcode : d.name == dev.name;
        });
        if (dev.ir_length != 0) // instruction_length 0 means "undefine"
            srv_.defined.push_back(dev);
        ++srv_.defined_tag;
        return Err::no_error;
    }
    ChainRec * ch = srv_.find_chain(chain_id);
    if (ch == nullptr)
        return Err::invalid_chain_id;
    if (!owns_chain(*ch))
        return Err::not_locked;
    if (tap_pos >= ch->model.taps().size())
        return Err::bad_tap_position;
    Tap & t = ch->model.taps()[tap_pos];
    t.ir_length = dev.ir_length;
    t.features = dev.features;
    t.name = dev.name;
    ch->tag = srv_.next_tag++;
    return Err::no_error;
}

Err Session::cmd_get_defined_devices(Ctx & c)
{
    const std::uint32_t known_tag = c.in.u32();
    if (!c.exec)
        return Err::no_error;
    Writer list;
    std::uint32_t tag, n;
    {
        std::lock_guard lk(srv_.mu);
        tag = srv_.defined_tag;
        n = static_cast<std::uint32_t>(srv_.defined.size());
        if (known_tag != tag)
            for (const DefinedDevice & d : srv_.defined) {
                list.u32(d.idcode);
                list.u32(d.ir_length);
                list.u32(d.features);
                list.u32(d.mask);
                list.u32(0);
                list.str(d.name);
            }
    }
    if (!list.empty())
        fifo_write(0, list.bytes());
    c.out.u32(tag);
    c.out.u32(n);
    c.out.u32(static_cast<std::uint32_t>(list.size()));
    return Err::no_error;
}

Err Session::cmd_set_parameter(Ctx & c)
{
    const std::uint32_t chain_id = c.in.u32();
    const std::string name = c.in.str();
    const std::uint32_t value = c.in.u32();
    if (!c.exec)
        return Err::no_error;
    std::lock_guard lk(srv_.mu);
    ChainRec * ch = srv_.find_chain(chain_id);
    if (ch == nullptr)
        return Err::invalid_chain_id;
    if (!owns_chain(*ch))
        return Err::not_locked;
    ch->cable.parameters[name] = value;
    return Err::no_error;
}

Err Session::cmd_get_parameter(Ctx & c)
{
    const std::uint32_t chain_id = c.in.u32();
    const std::string name = c.in.str();
    if (!c.exec)
        return Err::no_error;
    std::lock_guard lk(srv_.mu);
    ChainRec * ch = srv_.find_chain(chain_id);
    if (ch == nullptr)
        return Err::invalid_chain_id;
    if (!owns_chain(*ch))
        return Err::not_locked;
    auto it = ch->cable.parameters.find(name);
    if (it == ch->cable.parameters.end())
        return Err::invalid_parameter; // [inferred]
    c.out.u32(it->second);
    return Err::no_error;
}

Err Session::cmd_open_device(Ctx & c)
{
    const std::uint32_t chain_id = c.in.u32();
    const std::uint32_t tap_pos = c.in.u32();
    std::vector<Claim> claims;
    if (version_ >= 13) {
        const std::uint32_t n = c.in.u32();
        for (std::uint32_t i = 0; i < n; ++i) {
            Claim cl;
            cl.type = c.in.u32();
            cl.length = c.in.u32();
            cl.value = c.in.u64();
            claims.push_back(cl);
        }
    } else if (version_ >= 2) {
        const std::uint32_t n = c.in.u32();
        for (std::uint32_t i = 0; i < n; ++i) {
            const std::uint32_t type = c.in.u32();
            claims.push_back({type, 0, c.in.u32()});
        }
    } else {
        const std::uint32_t n = c.in.u32();
        c.in.u32();
        for (std::uint32_t i = 0; i < n; ++i)
            claims.push_back({claim::ir, 0, c.in.u32()});
    }
    const std::string app = c.in.str();
    if (!c.exec)
        return Err::no_error;

    std::lock_guard lk(srv_.mu);
    ChainRec * ch = srv_.find_chain(chain_id);
    if (ch == nullptr)
        return Err::invalid_chain_id;
    if (!owns_chain(*ch))
        return Err::not_locked;
    if (tap_pos >= ch->model.taps().size())
        return Err::bad_tap_position;
    const std::size_t chain_index = ch->index;
    for (const auto & [id, other] : srv_.opens) {
        if (other.chain != chain_index || other.tap != tap_pos || other.whole_chain)
            continue;
        for (const Claim & mine : claims)
            for (const Claim & theirs : other.claims)
                if (claims_conflict(mine, theirs)) {
                    write_blocker(c.out, Owner{other.session, other.id});
                    return Err::chains_claimed;
                }
    }
    const std::uint32_t id = srv_.next_open_id++;
    srv_.opens.emplace(id, OpenRec{id, id_, chain_index, tap_pos, false, std::move(claims), app});
    c.out.u32(id);
    return Err::no_error;
}

Err Session::cmd_open_entire_chain(Ctx & c)
{
    const std::uint32_t chain_id = c.in.u32();
    c.in.u32(); // style - 1; only JTAG exists
    const std::string app = c.in.str();
    if (!c.exec)
        return Err::no_error;
    std::lock_guard lk(srv_.mu);
    ChainRec * ch = srv_.find_chain(chain_id);
    if (ch == nullptr)
        return Err::invalid_chain_id;
    if (!owns_chain(*ch))
        return Err::not_locked;
    const std::uint32_t id = srv_.next_open_id++;
    const std::size_t chain_index = ch->index;
    srv_.opens.emplace(id, OpenRec{id, id_, chain_index, 0, true, {}, app});
    c.out.u32(id);
    return Err::no_error;
}

// ---------------------------------------------------------------- device level

Err Session::cmd_close_device(Ctx & c)
{
    const std::uint32_t open_id = c.in.u32();
    if (!c.exec)
        return Err::no_error;
    {
        std::lock_guard lk(srv_.mu);
        OpenRec * open = srv_.find_open(open_id, id_);
        if (open == nullptr)
            return Err::invalid_open_id;
        ChainRec & ch = srv_.chains[open->chain];
        if (ch.owner.session == id_ && ch.owner.open_id == open_id)
            ch.owner = {};
        srv_.opens.erase(open_id);
    }
    srv_.cv.notify_all();
    return Err::no_error;
}

Err Session::cmd_lock_device(Ctx & c)
{
    const std::uint32_t open_id = c.in.u32();
    const std::uint32_t timeout = c.in.u32();
    if (!c.exec)
        return Err::no_error;
    std::unique_lock lk(srv_.mu);
    OpenRec * open = srv_.find_open(open_id, id_);
    if (open == nullptr)
        return Err::invalid_open_id;
    return acquire(lk, srv_.chains[open->chain], open_id, timeout, c.out);
}

Err Session::cmd_unlock_device(Ctx & c)
{
    const std::uint32_t open_id = c.in.u32();
    if (!c.exec)
        return Err::no_error;
    {
        std::lock_guard lk(srv_.mu);
        OpenRec * open = srv_.find_open(open_id, id_);
        if (Err e = check_device_lock(open); e != Err::no_error)
            return e;
        srv_.chains[open->chain].owner = {};
    }
    srv_.cv.notify_all();
    return Err::no_error;
}

Err Session::cmd_unlock_lock_device(Ctx & c)
{
    // Either id may be a chain id or an open id: this one opcode implements
    // open->open, chain->open and open->chain lock handover.
    const std::uint32_t unlock_id = c.in.u32();
    const std::uint32_t lock_id = c.in.u32();
    if (!c.exec)
        return Err::no_error;

    std::lock_guard lk(srv_.mu);
    auto resolve = [&](std::uint32_t id) -> std::optional<std::pair<std::size_t, std::uint32_t>> {
        if (id < open_id_base) {
            if (ChainRec * ch = srv_.find_chain(id))
                return std::pair{ch->index, std::uint32_t{0}};
            return std::nullopt;
        }
        if (OpenRec * open = srv_.find_open(id, id_))
            return std::pair{open->chain, id};
        return std::nullopt;
    };
    const auto from = resolve(unlock_id);
    const auto to = resolve(lock_id);
    if (!from || !to)
        return Err::invalid_open_id;
    if (from->first != to->first)
        return Err::invalid_parameter;
    ChainRec & ch = srv_.chains[from->first];
    if (ch.owner.session != id_ || ch.owner.open_id != from->second)
        return Err::not_locked;
    ch.owner.open_id = to->second;
    return Err::no_error;
}

Err Session::cmd_access_ir(Ctx & c)
{
    const std::uint32_t open_id = c.in.u32();
    const std::uint32_t instruction = c.in.u32();
    c.in.u32(); // flags: bit0 = caller wants the capture; we always send it
    if (!c.exec)
        return Err::no_error;
    std::lock_guard lk(srv_.mu);
    OpenRec * open = srv_.find_open(open_id, id_);
    if (Err e = check_device_lock(open); e != Err::no_error)
        return e;
    if (open->whole_chain)
        return Err::invalid_parameter;
    ChainRec & ch = srv_.chains[open->chain];
    if (instruction >> ch.model.taps()[open->tap].ir_length)
        return Err::invalid_parameter;
    c.out.u32(ch.model.scan_ir(open->tap, instruction));
    return Err::no_error;
}

Err Session::cmd_access_ir_fifo(Ctx & c)
{
    const std::uint32_t open_id = c.in.u32();
    const std::uint32_t flags = c.in.u32();
    const std::uint32_t length = c.in.u32();
    const auto tx = fifo_read(0, bytes_for(length));
    if (!c.exec)
        return Err::no_error;
    std::vector<std::uint8_t> rx;
    {
        std::lock_guard lk(srv_.mu);
        OpenRec * open = srv_.find_open(open_id, id_);
        if (Err e = check_device_lock(open); e != Err::no_error)
            return e;
        ChainRec & ch = srv_.chains[open->chain];
        const Bits in = Bits::from_bytes(tx, length);
        Bits out;
        if (open->whole_chain) {
            out = ch.model.scan_ir_chain(in);
        } else {
            if (length != ch.model.taps()[open->tap].ir_length)
                return Err::invalid_parameter;
            out = Bits::from_u64(ch.model.scan_ir(open->tap, static_cast<std::uint32_t>(in.to_u64())), length);
        }
        rx = out.to_bytes();
    }
    if (flags & 1)
        fifo_write(0, rx);
    return Err::no_error;
}

Err Session::cmd_access_dr(Ctx & c)
{
    const std::uint32_t open_id = c.in.u32();
    const std::uint32_t length = c.in.u32();
    c.in.u32(); // AJI_DR_FLAGS: unspecified bits are always written as 0 here
    const std::uint32_t w_off = c.in.u32();
    const std::uint32_t w_len = c.in.u32();
    const std::uint32_t r_off = c.in.u32();
    const std::uint32_t r_len = c.in.u32();
    const std::uint32_t batch = version_ >= 5 ? c.in.u32() : 1;
    const std::size_t w_bytes = bytes_for(w_len);
    const auto tx = w_len > 0 ? fifo_read(0, w_bytes * batch) : std::vector<std::uint8_t>{};
    if (!c.exec)
        return Err::no_error;
    if (batch == 0 || w_off + w_len > length || r_off + r_len > length)
        return Err::invalid_parameter;

    std::vector<std::uint8_t> rx;
    {
        std::lock_guard lk(srv_.mu);
        OpenRec * open = srv_.find_open(open_id, id_);
        if (Err e = check_device_lock(open); e != Err::no_error)
            return e;
        ChainRec & ch = srv_.chains[open->chain];
        for (std::uint32_t b = 0; b < batch; ++b) {
            Bits in(length);
            if (w_len > 0)
                in.splice(w_off, Bits::from_bytes(std::span(tx).subspan(b * w_bytes, w_bytes), w_len));
            const Bits out = open->whole_chain ? ch.model.scan_dr_chain(in) : ch.model.scan_dr(open->tap, in);
            if (r_len > 0) {
                const auto bytes = out.slice(r_off, r_len).to_bytes();
                rx.insert(rx.end(), bytes.begin(), bytes.end());
            }
        }
    }
    if (!rx.empty())
        fifo_write(0, rx);
    if (version_ >= 11) {
        using namespace std::chrono;
        c.out.u32(static_cast<std::uint32_t>(
            duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count()));
    }
    return Err::no_error;
}

Err Session::cmd_any_sequence(Ctx & c)
{
    const std::uint32_t open_id = c.in.u32();
    const std::uint32_t tcks = c.in.u32();
    const bool want_tdo = c.in.u32() != 0;
    const auto tdi = fifo_read(0, bytes_for(tcks));
    fifo_read(1, bytes_for(tcks)); // TMS: no TAP state machine is modelled
    if (!c.exec)
        return Err::no_error;
    {
        std::lock_guard lk(srv_.mu);
        if (Err e = check_device_lock(srv_.find_open(open_id, id_)); e != Err::no_error)
            return e;
    }
    if (want_tdo)
        fifo_write(0, tdi); // TDO = TDI: deterministic, good enough for transport tests
    return Err::no_error;
}

Err Session::cmd_simple_device_op(Ctx & c, Cmd cmd)
{
    const std::uint32_t open_id = c.in.u32();
    if (cmd == Cmd::run_test_idle) {
        c.in.u32(); // clocks
        if (version_ >= 12)
            c.in.u32(); // flags
    } else if (cmd == Cmd::delay_microseconds) {
        c.in.u32(); // not actually slept, to keep tests fast
    }
    if (!c.exec)
        return Err::no_error;
    std::lock_guard lk(srv_.mu);
    OpenRec * open = srv_.find_open(open_id, id_);
    if (Err e = check_device_lock(open); e != Err::no_error)
        return e;
    if (cmd == Cmd::test_logic_reset)
        srv_.chains[open->chain].model.test_logic_reset();
    return Err::no_error;
}

} // namespace

// ---------------------------------------------------------------- server

Server::Impl::Impl(Scenario s, ServerOptions o) : scenario(std::move(s)), options(std::move(o))
{
    if (scenario.version < 1 || scenario.version > current_version)
        throw std::invalid_argument(std::format("unsupported protocol version {}", scenario.version));
    for (const Cable & cable : scenario.cables)
        chains.push_back({chains.size(), cable, ChainModel(cable.taps), 0, false, {}});

    listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0)
        throw std::system_error(errno, std::generic_category(), "socket");
    int one = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(options.port);
    if (::inet_pton(AF_INET, options.bind_address.c_str(), &addr.sin_addr) != 1)
        throw std::invalid_argument("bad bind address: " + options.bind_address);
    if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 || ::listen(listen_fd, 8) < 0) {
        const int err = errno;
        ::close(listen_fd);
        throw std::system_error(err, std::generic_category(), "bind/listen");
    }
    socklen_t len = sizeof(addr);
    ::getsockname(listen_fd, reinterpret_cast<sockaddr *>(&addr), &len);
    bound_port = ntohs(addr.sin_port);
    acceptor = std::jthread([this] { accept_loop(); });
}

void Server::Impl::accept_loop()
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
        std::lock_guard lk(mu);
        if (stopping) {
            ::close(fd);
            break;
        }
        const unsigned id = ++session_count;
        session_fds[id] = fd;
        sessions.emplace_back([this, fd, id] { Session(*this, fd, id).run(); });
    }
}

void Server::Impl::stop()
{
    if (stopping.exchange(true))
        return;
    ::shutdown(listen_fd, SHUT_RDWR);
    if (acceptor.joinable())
        acceptor.join();
    ::close(listen_fd);
    {
        std::lock_guard lk(mu);
        for (const auto & [id, fd] : session_fds)
            ::shutdown(fd, SHUT_RDWR);
    }
    cv.notify_all();
    for (auto & t : sessions)
        if (t.joinable())
            t.join();
}

Server::Server(Scenario scenario, ServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(scenario), std::move(options)))
{
}

Server::~Server() = default;

std::uint16_t Server::port() const { return impl_->bound_port; }
void Server::stop() { impl_->stop(); }

void Server::inject(Fault fault)
{
    std::lock_guard lk(impl_->mu);
    impl_->faults.push_back(fault);
}

std::vector<CommandRecord> Server::commands() const
{
    std::lock_guard lk(impl_->mu);
    return impl_->records;
}

std::vector<std::string> Server::take_model_errors()
{
    std::lock_guard lk(impl_->mu);
    std::vector<std::string> out;
    for (auto & ch : impl_->chains)
        for (auto & e : ch.model.take_errors())
            out.push_back(std::format("{}: {}", ch.cable.hw_name, std::move(e)));
    return out;
}

unsigned Server::connections() const
{
    std::lock_guard lk(impl_->mu);
    return impl_->session_count;
}

} // namespace mock_jtagd
