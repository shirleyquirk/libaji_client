#include "jtag_proxy/transcript.hpp"

#include <algorithm>
#include <format>

namespace jtag_proxy {
namespace {

bool selected(const ScanRecord & s, const ChannelSelector & sel, std::uint32_t ir)
{
    if (s.op != ScanRecord::Op::dr || !s.executed || s.result != mock_jtagd::Err::no_error)
        return false;
    if (sel.node_idcode)
        return s.node_select && s.node_idcode == *sel.node_idcode && s.node_ir == ir;
    return !s.node_select && s.tap == sel.tap && s.ir == ir;
}

std::string word_list(const std::vector<std::uint64_t> & words, std::uint32_t word_bits)
{
    std::string out;
    for (std::uint64_t w : words)
        out += std::format("{}0x{:0{}X}", out.empty() ? "" : ", ", w, (word_bits + 3) / 4);
    return out;
}

} // namespace

TranscriptResult extract_transcript(const std::vector<ScanRecord> & scans, const ChannelSelector & sel)
{
    TranscriptResult result;
    std::vector<mock_jtagd::Exchange> all;
    mock_jtagd::Exchange cur;

    auto close = [&] {
        if (!cur.request.empty())
            all.push_back(cur);
        cur = {};
    };
    for (const ScanRecord & s : scans) {
        const bool cmd = selected(s, sel, sel.command_ir);
        const bool resp = selected(s, sel, sel.response_ir);
        if (!cmd && !resp)
            continue;
        const BitWindow & data = cmd ? s.write : s.read;
        if (data.bits % sel.word_bits != 0)
            result.warnings.push_back(std::format("scan #{}: {} bits is not a whole number of {}-bit words", s.seq,
                                                  data.bits, sel.word_bits));
        auto words = data.words(sel.word_bits);
        if (cmd) {
            if (!cur.response.empty())
                close();
            cur.request.insert(cur.request.end(), words.begin(), words.end());
        } else {
            if (cur.request.empty()) {
                result.warnings.push_back(std::format("scan #{}: response read before any request; ignored", s.seq));
                continue;
            }
            for (std::uint64_t w : words)
                if (!(sel.strip_leading_idle && cur.response.empty() && w == *sel.strip_leading_idle))
                    cur.response.push_back(w);
        }
    }
    close();

    for (const auto & ex : all) {
        auto same_request = std::ranges::find_if(result.exchanges, [&](const auto & e) { return e.request == ex.request; });
        if (same_request == result.exchanges.end())
            result.exchanges.push_back(ex);
        else if (same_request->response != ex.response)
            result.warnings.push_back(std::format(
                "request [{}] got different responses ([{}] first, then [{}]); the mock replays the first",
                word_list(ex.request, sel.word_bits), word_list(same_request->response, sel.word_bits),
                word_list(ex.response, sel.word_bits)));
    }
    return result;
}

std::string transcript_yaml(const TranscriptResult & t, const ChannelSelector & sel, std::string_view name,
                            std::string_view source)
{
    std::string out = std::format("# Captured by jtag-proxy from {}\n", source);
    if (sel.node_idcode)
        out += std::format("# node {:08X}, command IR 0x{:X}, response IR 0x{:X}\n", *sel.node_idcode, sel.command_ir,
                           sel.response_ir);
    else
        out += std::format("# tap {}, command IR 0x{:X}, response IR 0x{:X}\n", sel.tap.value_or(0), sel.command_ir,
                           sel.response_ir);
    for (const auto & w : t.warnings)
        out += std::format("# WARNING: {}\n", w);
    out += std::format("channels:\n  - name: {}\n    command_ir: 0x{:X}\n    response_ir: 0x{:X}\n"
                       "    word_bits: {}\n    provenance: observed\n    transcript:\n",
                       name, sel.command_ir, sel.response_ir, sel.word_bits);
    if (t.exchanges.empty())
        out += "      []\n";
    for (const auto & ex : t.exchanges)
        out += std::format("      - request:  [{}]\n        response: [{}]\n", word_list(ex.request, sel.word_bits),
                           word_list(ex.response, sel.word_bits));
    return out;
}

} // namespace jtag_proxy
