// Passive decoder for one client<->jtagd connection. Fed copies of the bytes
// in each direction; emits logical scans and wire-log lines. Never touches
// sockets, so it can't affect forwarding.
#pragma once

#include "jtag_proxy/scan.hpp"
#include "mock_jtagd/protocol.hpp"
#include "mock_jtagd/scenario.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace jtag_proxy {

class Decoder {
public:
    struct Sinks {
        std::function<void(ScanRecord)> scan;
        std::function<void(std::string)> wire;
        std::function<void(std::string)> error;
    };

    Decoder(unsigned session, const mock_jtagd::Scenario * names, Sinks sinks,
            std::chrono::steady_clock::time_point t0, std::atomic<std::uint64_t> & seq);

    void from_client(std::span<const std::uint8_t> bytes);
    void from_server(std::span<const std::uint8_t> bytes);
    // Report bytes left over in FIFO queues: a sign the decoder's idea of who
    // sends FIFO data when doesn't match the server's.
    void finish();

private:
    class Reassembler {
    public:
        template <class F> void feed(std::span<const std::uint8_t> bytes, F && on_packet)
        {
            buf_.insert(buf_.end(), bytes.begin(), bytes.end());
            for (;;) {
                if (buf_.size() < 2)
                    return;
                const unsigned header = (unsigned{buf_[0]} << 8) | buf_[1];
                const std::size_t len = (header & 0x0FFF) + 1;
                if (buf_.size() < 2 + len)
                    return;
                std::vector<std::uint8_t> payload(buf_.begin() + 2, buf_.begin() + 2 + static_cast<std::ptrdiff_t>(len));
                buf_.erase(buf_.begin(), buf_.begin() + 2 + static_cast<std::ptrdiff_t>(len));
                on_packet(header >> 12, std::move(payload));
            }
        }

    private:
        std::vector<std::uint8_t> buf_;
    };

    struct Op {
        mock_jtagd::Cmd cmd;
        std::vector<std::uint8_t> args;
        std::vector<std::uint8_t> resp;
        mock_jtagd::Err err = mock_jtagd::Err::no_error;
        bool executed = false;
        unsigned packet = 0;
        bool ok() const { return executed && err == mock_jtagd::Err::no_error; }
    };
    struct Needs {
        std::size_t tx0 = 0, tx1 = 0, rx0 = 0;
    };

    struct TapInfo {
        std::uint32_t idcode = 0, ir_length = 0, features = 0;
        std::string name;
    };
    struct HubInfo {
        std::vector<std::uint32_t> nibbles; // since the last HUB_INFO selection
        std::uint32_t idcode = 0;           // 0 until 8 nibbles seen
        std::vector<std::uint32_t> nodes;
        std::uint32_t ir_bits() const { return idcode & 0xFF; }
        std::uint32_t node_count() const { return (idcode >> 19) & 0xFF; }
        std::uint32_t select_bits() const;
    };
    struct TapState {
        std::optional<std::uint32_t> ir;
        bool select_known = true;
        std::uint32_t select = 0, node_ir = 0;
        HubInfo hub;
    };
    struct Chain {
        std::string cable;
        std::vector<TapInfo> taps;
        std::vector<TapState> state;
    };
    struct Open {
        std::uint32_t chain = 0;
        std::optional<std::uint32_t> tap; // none: whole chain
        std::string app;
    };

    void client_packet(unsigned mux, std::vector<std::uint8_t> payload);
    void server_packet(unsigned mux, std::vector<std::uint8_t> payload);
    void pump();
    Needs needs(const Op & op) const;
    void process(const Op & op, std::vector<std::uint8_t> tx0, std::vector<std::uint8_t> tx1,
                 std::vector<std::uint8_t> rx0);
    void dr_scans(const Op & op, const std::vector<std::uint8_t> & tx, const std::vector<std::uint8_t> & rx);
    void hub_user1(TapState & ts, std::uint32_t length, const BitWindow & write);
    void hub_user0_info(TapState & ts, const BitWindow & read);
    bool hub_capable(const TapInfo & t) const;
    ScanRecord base_record(const Op & op, const Open & open) const;
    std::string node_name(std::uint32_t idcode, std::uint32_t select) const;
    std::vector<std::uint8_t> take(std::deque<std::uint8_t> & q, std::size_t n);
    void fail(std::string what);

    unsigned session_;
    const mock_jtagd::Scenario * names_;
    Sinks sinks_;
    std::chrono::steady_clock::time_point t0_;
    std::atomic<std::uint64_t> & seq_;

    bool broken_ = false;
    bool greeted_ = false;
    std::uint32_t greeting_version_ = 0;
    std::uint32_t version_ = 0;
    unsigned client_packets_ = 0;
    Reassembler from_client_, from_server_;
    std::deque<std::pair<unsigned, std::vector<std::uint8_t>>> pending_; // (packet no, mux-0 payload)
    std::deque<Op> ops_;
    std::array<std::deque<std::uint8_t>, mock_jtagd::mux_count - mock_jtagd::fifo_mux_base> tx_, rx_;

    std::map<std::uint32_t, Chain> chains_;
    std::map<std::uint32_t, Open> opens_;
};

} // namespace jtag_proxy
