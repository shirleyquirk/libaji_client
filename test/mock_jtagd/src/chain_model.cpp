#include "chain_model.hpp"

#include <algorithm>
#include <bit>
#include <format>
#include <optional>
#include <tuple>

namespace mock_jtagd {

std::uint32_t SldHub::idcode() const
{
    return ((hub_version & 0x1F) << 27) | ((static_cast<std::uint32_t>(nodes.size()) & 0xFF) << 19) |
           ((mfg & 0x7FF) << 8) | (ir_bits & 0xFF);
}

std::uint32_t SldHub::select_bits() const
{
    // ceil(log2(n + 1)), matching the client's ceil_log2(idcode_n + 1).
    const auto n = static_cast<std::uint32_t>(nodes.size()) + 1;
    return n <= 1 ? 0 : static_cast<std::uint32_t>(std::bit_width(n - 1));
}

Bits shift_through(Bits & captured, const Bits & in)
{
    const std::size_t r = captured.size();
    const std::size_t l = in.size();
    Bits out(l);
    for (std::size_t i = 0; i < l; ++i)
        out.set(i, i < r ? captured[i] : in[i - r]);

    // After l shifts the register holds the last r bits of the stream captured ++ in.
    Bits stream = captured;
    stream.append(in);
    captured = stream.slice(stream.size() - r, r);
    return out;
}

ChainModel::ChainModel(std::vector<Tap> taps) : taps_(std::move(taps)), state_(taps_.size())
{
    test_logic_reset();
}

void ChainModel::test_logic_reset()
{
    for (std::size_t i = 0; i < taps_.size(); ++i) {
        state_[i].ir = taps_[i].idcode_instruction;
        state_[i].hub = {};
        state_[i].seq_pos.clear();
    }
}

bool ChainModel::is_bypass(std::size_t tap) const
{
    const std::uint32_t ones = (taps_[tap].ir_length >= 32) ? ~0u : ((1u << taps_[tap].ir_length) - 1);
    return state_[tap].ir == ones;
}

std::uint32_t ChainModel::scan_ir(std::size_t tap, std::uint32_t instruction)
{
    state_.at(tap).ir = instruction;
    return taps_[tap].ir_capture;
}

ChainModel::RegKey ChainModel::key(std::size_t tap) const
{
    const TapState & s = state_[tap];
    return {s.ir, s.hub.select, s.hub.node_ir};
}

ChainModel::Target ChainModel::target(std::size_t tap) const
{
    const Tap & t = taps_[tap];
    const TapState & s = state_[tap];
    Target out;
    if (t.hub && s.ir == user0_instruction) {
        if (s.hub.select == 0 || s.hub.select > t.hub->nodes.size())
            return out;
        out.node_index = s.hub.select - 1;
        out.node = &t.hub->nodes[out.node_index];
        for (std::size_t i = 0; i < out.node->channels.size(); ++i) {
            const Channel & ch = out.node->channels[i];
            if (s.hub.node_ir == ch.command_ir || s.hub.node_ir == ch.response_ir) {
                out.channel = &ch;
                out.channel_index = i;
                out.command = s.hub.node_ir == ch.command_ir;
                return out;
            }
        }
        if (auto it = out.node->registers.find(s.hub.node_ir); it != out.node->registers.end())
            out.reg = &it->second;
        return out;
    }
    if (t.hub && s.ir == user1_instruction)
        return out;
    for (std::size_t i = 0; i < t.channels.size(); ++i) {
        const Channel & ch = t.channels[i];
        if (s.ir == ch.command_ir || s.ir == ch.response_ir) {
            out.channel = &ch;
            out.channel_index = i;
            out.command = s.ir == ch.command_ir;
            return out;
        }
    }
    if (auto it = t.registers.find(s.ir); it != t.registers.end())
        out.reg = &it->second;
    return out;
}

std::size_t ChainModel::dr_length(std::size_t tap) const
{
    const Tap & t = taps_[tap];
    const TapState & s = state_[tap];
    if (is_bypass(tap))
        return 1;
    if (s.ir == t.idcode_instruction)
        return 32;
    if (t.hub && s.ir == user1_instruction)
        return t.hub->select_bits() + t.hub->ir_bits;
    if (t.hub && s.ir == user0_instruction && s.hub.select == 0 && s.hub.node_ir == 0)
        return 4;
    if (const Target tg = target(tap); tg.reg)
        return tg.reg->length;
    return 0;
}

std::vector<std::uint32_t> ChainModel::hub_rom(const SldHub & hub) const
{
    std::vector<std::uint32_t> words{hub.idcode()};
    for (const SldNode & n : hub.nodes)
        words.push_back(n.idcode);
    std::vector<std::uint32_t> nibbles;
    for (std::uint32_t w : words)
        for (int i = 0; i < 8; ++i)
            nibbles.push_back((w >> (4 * i)) & 0xF);
    return nibbles;
}

Bits ChainModel::capture(std::size_t tap, std::size_t length)
{
    const Tap & t = taps_[tap];
    TapState & s = state_[tap];
    if (is_bypass(tap))
        return Bits(1);
    if (s.ir == t.idcode_instruction)
        return Bits::from_u64(t.idcode, 32);
    if (t.hub && s.ir == user1_instruction) {
        const std::uint64_t v = (std::uint64_t{s.hub.select} << t.hub->ir_bits) | s.hub.node_ir;
        return Bits::from_u64(v, t.hub->select_bits() + t.hub->ir_bits);
    }
    if (t.hub && s.ir == user0_instruction && s.hub.select == 0 && s.hub.node_ir == 0) {
        // HUB_INFO: each DR capture presents the next nibble of the hub ROM.
        const auto rom = hub_rom(*t.hub);
        const std::uint32_t nibble = rom[s.hub.rom_pos % rom.size()];
        ++s.hub.rom_pos;
        return Bits::from_u64(nibble, 4);
    }

    const Target tg = target(tap);
    if (tg.channel && !tg.command) {
        // Response side: present as many queued words as the scan has room for.
        auto & q = s.channels[{tg.node_index, tg.channel_index}].response;
        const std::size_t w = tg.channel->word_bits;
        Bits out;
        for (std::size_t i = 0; i < length / w; ++i) {
            std::uint64_t word = tg.channel->idle;
            if (!q.empty()) {
                word = q.front();
                q.pop_front();
            }
            out.append(Bits::from_u64(word, w));
        }
        out.resize(length);
        return out;
    }
    if (tg.channel)
        return Bits(length);
    if (tg.reg && tg.reg->kind == RegisterSpec::Kind::constant)
        return Bits::from_u64(tg.reg->value, length);
    if (tg.reg && tg.reg->kind == RegisterSpec::Kind::sequence) {
        std::size_t & pos = s.seq_pos[key(tap)];
        const std::uint64_t v = tg.reg->values[std::min(pos, tg.reg->values.size() - 1)];
        ++pos;
        return Bits::from_u64(v, length);
    }
    auto it = s.regs.find(key(tap));
    Bits reg = it != s.regs.end() ? it->second : Bits::from_u64(tg.reg ? tg.reg->value : 0, length);
    reg.resize(length);
    return reg;
}

void ChainModel::channel_receive(std::size_t tap, const Target & tg, const Bits & value)
{
    const Channel & ch = *tg.channel;
    const std::string where = tg.node ? std::format("node '{}' channel '{}'", tg.node->name, ch.name)
                                      : std::format("device '{}' channel '{}'", taps_[tap].name, ch.name);
    if (value.size() % ch.word_bits != 0) {
        errors_.push_back(std::format("{}: command scan of {} bits is not a whole number of {}-bit words",
                                      where, value.size(), ch.word_bits));
        return;
    }
    ChannelState & cs = state_[tap].channels[{tg.node_index, tg.channel_index}];
    for (std::size_t off = 0; off < value.size(); off += ch.word_bits)
        cs.request.push_back(value.slice(off, ch.word_bits).to_u64());

    for (const Exchange & ex : ch.transcript)
        if (ex.request == cs.request) {
            cs.response.insert(cs.response.end(), ex.response.begin(), ex.response.end());
            cs.request.clear();
            return;
        }
    const bool prefix_of_some = std::ranges::any_of(ch.transcript, [&](const Exchange & ex) {
        return ex.request.size() > cs.request.size() &&
               std::equal(cs.request.begin(), cs.request.end(), ex.request.begin());
    });
    if (!prefix_of_some) {
        std::string words;
        for (std::uint64_t w : cs.request)
            words += std::format("{}0x{:0{}X}", words.empty() ? "" : ", ", w, (ch.word_bits + 3) / 4);
        errors_.push_back(std::format("{}: no transcript entry for request [{}]", where, words));
        cs.request.clear();
    }
}

void ChainModel::update(std::size_t tap, const Bits & value)
{
    const Tap & t = taps_[tap];
    TapState & s = state_[tap];
    if (is_bypass(tap) || s.ir == t.idcode_instruction)
        return;
    if (t.hub && s.ir == user1_instruction) {
        const std::uint64_t v = value.to_u64();
        s.hub.node_ir = static_cast<std::uint32_t>(v & ((std::uint64_t{1} << t.hub->ir_bits) - 1));
        s.hub.select = static_cast<std::uint32_t>(v >> t.hub->ir_bits);
        if (s.hub.select == 0 && s.hub.node_ir == 0)
            s.hub.rom_pos = 0;
        return;
    }
    if (t.hub && s.ir == user0_instruction && s.hub.select == 0 && s.hub.node_ir == 0)
        return;
    const Target tg = target(tap);
    if (tg.channel) {
        if (tg.command)
            channel_receive(tap, tg, value);
        return;
    }
    if (tg.reg && tg.reg->kind != RegisterSpec::Kind::rw)
        return;
    s.regs[key(tap)] = value;
}

Bits ChainModel::scan_dr(std::size_t tap, const Bits & in)
{
    std::size_t len = dr_length(tap);
    if (len == 0)
        len = in.size();
    Bits reg = capture(tap, len);
    Bits out = shift_through(reg, in);
    update(tap, reg);
    return out;
}

Bits ChainModel::scan_ir_chain(const Bits & in)
{
    Bits captured;
    for (const Tap & t : taps_)
        captured.append(Bits::from_u64(t.ir_capture, t.ir_length));
    Bits reg = captured;
    Bits out = shift_through(reg, in);
    std::size_t offset = 0;
    for (std::size_t i = 0; i < taps_.size(); ++i) {
        state_[i].ir = static_cast<std::uint32_t>(reg.slice(offset, taps_[i].ir_length).to_u64());
        offset += taps_[i].ir_length;
    }
    return out;
}

Bits ChainModel::scan_dr_chain(const Bits & in)
{
    // A free-length register (plain R/W DR) takes whatever scan length the
    // fixed-length ones leave over; with several, the first one gets it all.
    std::vector<std::size_t> lens(taps_.size());
    std::size_t fixed = 0;
    std::optional<std::size_t> free_tap;
    for (std::size_t i = 0; i < taps_.size(); ++i) {
        lens[i] = dr_length(i);
        if (lens[i] == 0 && !free_tap)
            free_tap = i;
        fixed += lens[i];
    }
    if (free_tap)
        lens[*free_tap] = in.size() > fixed ? in.size() - fixed : 1;

    Bits reg;
    for (std::size_t i = 0; i < taps_.size(); ++i)
        reg.append(lens[i] ? capture(i, lens[i]) : Bits());
    Bits out = shift_through(reg, in);
    std::size_t offset = 0;
    for (std::size_t i = 0; i < taps_.size(); ++i) {
        if (lens[i])
            update(i, reg.slice(offset, lens[i]));
        offset += lens[i];
    }
    return out;
}

} // namespace mock_jtagd
