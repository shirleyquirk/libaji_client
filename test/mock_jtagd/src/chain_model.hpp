// Behavioural model of a JTAG chain: per-TAP instruction register plus data
// registers with shift-register semantics. Not a TAP state machine: the AJI
// protocol works in whole IR/DR scans, so that's the granularity modelled.
#pragma once

#include "bits.hpp"
#include "mock_jtagd/scenario.hpp"

#include <cstdint>
#include <map>
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

private:
    struct HubState {
        std::uint32_t select = 0;   // 0 = hub itself, n = node n-1
        std::uint32_t node_ir = 0;  // 0 with select 0 = HUB_INFO
        std::size_t rom_pos = 0;
    };
    struct TapState {
        std::uint32_t ir = 0;
        HubState hub;
        // Plain read/write data registers, keyed by (instruction, hub select, node ir).
        std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, Bits> regs;
    };

    // Register length for the current instruction; 0 means "whatever is scanned".
    std::size_t dr_length(std::size_t tap) const;
    Bits capture(std::size_t tap, std::size_t length);
    void update(std::size_t tap, const Bits & value);
    std::vector<std::uint32_t> hub_rom(const SldHub & hub) const;
    bool is_bypass(std::size_t tap) const;

    std::vector<Tap> taps_;
    std::vector<TapState> state_;
};

// Shift `in` through a register currently holding `captured`; returns TDO bits
// and leaves the new register contents in `captured`.
Bits shift_through(Bits & captured, const Bits & in);

} // namespace mock_jtagd
