// Behavioural model of a JTAG chain: per-TAP instruction register plus data
// registers with shift-register semantics. Not a TAP state machine: the AJI
// protocol works in whole IR/DR scans, so that's the granularity modelled.
#pragma once

#include "bits.hpp"
#include "mock_jtagd/scenario.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace mock_jtagd {

class ChainModel {
public:
    explicit ChainModel(std::vector<Tap> taps);

    const std::vector<Tap> & taps() const { return taps_; }
    std::vector<Tap> & taps() { return taps_; }

    // Single-TAP scans (other TAPs implicitly in BYPASS).
    std::uint32_t scan_ir(std::size_t tap, std::uint32_t instruction);
    Bits scan_dr(std::size_t tap, const Bits & in);

    // Whole-chain scans: bit 0 is nearest TDO, i.e. TAP 0's LSB.
    Bits scan_ir_chain(const Bits & in);
    Bits scan_dr_chain(const Bits & in);

    void test_logic_reset();
    std::uint32_t instruction(std::size_t tap) const { return state_.at(tap).ir; }

    // Behaviour violations (e.g. a request no transcript entry matches),
    // drained by the server so tests can fail on them.
    std::vector<std::string> take_errors() { return std::exchange(errors_, {}); }

private:
    // Data register currently selected on a TAP: (instruction, hub select, node ir).
    using RegKey = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>;

    struct HubState {
        std::uint32_t select = 0;   // 0 = hub itself, n = node n-1
        std::uint32_t node_ir = 0;  // 0 with select 0 = HUB_INFO
        std::size_t rom_pos = 0;
    };
    struct ChannelState {
        std::vector<std::uint64_t> request;   // words received so far
        std::deque<std::uint64_t> response;   // words waiting to be read
    };
    struct TapState {
        std::uint32_t ir = 0;
        HubState hub;
        std::map<RegKey, Bits> regs;               // rw contents
        std::map<RegKey, std::size_t> seq_pos;     // sequence progress
        std::map<std::pair<std::size_t, std::size_t>, ChannelState> channels; // (node, channel)
    };
    // What a DR scan on `tap` currently reaches.
    static constexpr std::size_t tap_level = static_cast<std::size_t>(-1); // node_index for TAP channels

    struct Target {
        const RegisterSpec * reg = nullptr;
        const SldNode * node = nullptr;
        std::size_t node_index = tap_level;
        const Channel * channel = nullptr;
        std::size_t channel_index = 0;
        bool command = false;        // channel command (true) or response (false) side
    };

    Target target(std::size_t tap) const;
    // Register length for the current instruction; 0 means "whatever is scanned".
    std::size_t dr_length(std::size_t tap) const;
    Bits capture(std::size_t tap, std::size_t length);
    void update(std::size_t tap, const Bits & value);
    void channel_receive(std::size_t tap, const Target & t, const Bits & value);
    std::vector<std::uint32_t> hub_rom(const SldHub & hub) const;
    bool is_bypass(std::size_t tap) const;
    RegKey key(std::size_t tap) const;

    std::vector<Tap> taps_;
    std::vector<TapState> state_;
    std::vector<std::string> errors_;
};

// Shift `in` through a register currently holding `captured`; returns TDO bits
// and leaves the new register contents in `captured`.
Bits shift_through(Bits & captured, const Bits & in);

} // namespace mock_jtagd
