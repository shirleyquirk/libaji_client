// Turn captured scans into a mock-jtagd channel transcript.
#pragma once

#include "jtag_proxy/scan.hpp"
#include "mock_jtagd/scenario.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jtag_proxy {

// Which scans make up one command/response channel. A node channel is
// selected by node IDCODE and node IRs; a TAP channel by TAP position and
// TAP IRs.
struct ChannelSelector {
    std::optional<std::uint32_t> node_idcode;
    std::optional<std::uint32_t> tap;
    std::uint32_t command_ir = 0;
    std::uint32_t response_ir = 0;
    std::uint32_t word_bits = 32;
    // Drop words equal to this at the start of each response, e.g. the
    // "nothing yet" words a client reads while polling for a reply.
    std::optional<std::uint64_t> strip_leading_idle;
};

struct TranscriptResult {
    std::vector<mock_jtagd::Exchange> exchanges; // unique, first-seen order
    std::vector<std::string> warnings;           // ambiguities a human should look at
};

// Command-IR writes accumulate into a request; response-IR reads into its
// response; the next command-IR write after a response starts a new exchange.
TranscriptResult extract_transcript(const std::vector<ScanRecord> & scans, const ChannelSelector & sel);

// A `channels:` YAML snippet for a scenario file, provenance: observed.
std::string transcript_yaml(const TranscriptResult & t, const ChannelSelector & sel, std::string_view name,
                            std::string_view source);

} // namespace jtag_proxy
