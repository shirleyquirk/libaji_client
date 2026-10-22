#include "decoder.hpp"

#include "bits.hpp"

#include <algorithm>
#include <bit>
#include <format>

namespace jtag_proxy {

using mock_jtagd::Bits;
using mock_jtagd::Cmd;
using mock_jtagd::Err;
using mock_jtagd::Reader;

namespace {

std::size_t bytes_for(std::uint32_t bits) { return (std::size_t{bits} + 7) / 8; }

std::string hexdump(std::span<const std::uint8_t> bytes, std::size_t max = 64)
{
    std::string out;
    for (std::size_t i = 0; i < bytes.size() && i < max; ++i)
        out += std::format("{}{:02X}", (i && i % 4 == 0) ? " " : "", bytes[i]);
    if (bytes.size() > max)
        out += std::format(" ...(+{}B)", bytes.size() - max);
    return out;
}

BitWindow window(std::uint32_t offset, std::uint32_t bits, std::span<const std::uint8_t> bytes)
{
    BitWindow w{offset, bits, {}};
    w.bytes.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(std::min(bytes.size(), bytes_for(bits))));
    return w;
}

} // namespace

std::uint32_t Decoder::HubInfo::select_bits() const
{
    const std::uint32_t n = node_count() + 1;
    return n <= 1 ? 0 : static_cast<std::uint32_t>(std::bit_width(n - 1));
}

Decoder::Decoder(unsigned session, const mock_jtagd::Scenario * names, Sinks sinks,
                 std::chrono::steady_clock::time_point t0, std::atomic<std::uint64_t> & seq)
    : session_(session), names_(names), sinks_(std::move(sinks)), t0_(t0), seq_(seq)
{
}

void Decoder::fail(std::string what)
{
    if (broken_)
        return;
    broken_ = true;
    sinks_.error(std::format("s{}: decoding stopped: {}", session_, what));
}

std::vector<std::uint8_t> Decoder::take(std::deque<std::uint8_t> & q, std::size_t n)
{
    std::vector<std::uint8_t> out(q.begin(), q.begin() + static_cast<std::ptrdiff_t>(n));
    q.erase(q.begin(), q.begin() + static_cast<std::ptrdiff_t>(n));
    return out;
}

// ---------------------------------------------------------------- streams

void Decoder::from_client(std::span<const std::uint8_t> bytes)
{
    if (broken_)
        return;
    try {
        from_client_.feed(bytes, [&](unsigned mux, std::vector<std::uint8_t> p) { client_packet(mux, std::move(p)); });
        pump();
    } catch (const std::exception & e) {
        fail(e.what());
    }
}

void Decoder::from_server(std::span<const std::uint8_t> bytes)
{
    if (broken_)
        return;
    try {
        from_server_.feed(bytes, [&](unsigned mux, std::vector<std::uint8_t> p) { server_packet(mux, std::move(p)); });
        pump();
    } catch (const std::exception & e) {
        fail(e.what());
    }
}

void Decoder::finish()
{
    if (broken_)
        return;
    for (std::size_t i = 0; i < tx_.size(); ++i)
        if (!tx_[i].empty())
            sinks_.error(std::format("s{}: {} unconsumed client FIFO bytes on mux {}", session_, tx_[i].size(),
                                     i + mock_jtagd::fifo_mux_base));
    for (std::size_t i = 0; i < rx_.size(); ++i)
        if (!rx_[i].empty())
            sinks_.error(std::format("s{}: {} unconsumed server FIFO bytes on mux {}", session_, rx_[i].size(),
                                     i + mock_jtagd::fifo_mux_base));
    if (!ops_.empty())
        sinks_.error(std::format("s{}: {} commands still waiting for FIFO data at disconnect", session_, ops_.size()));
}

void Decoder::client_packet(unsigned mux, std::vector<std::uint8_t> payload)
{
    if (mux >= mock_jtagd::fifo_mux_base) {
        auto & q = tx_[mux - mock_jtagd::fifo_mux_base];
        q.insert(q.end(), payload.begin(), payload.end());
        return;
    }
    if (mux != mock_jtagd::mux_command) {
        sinks_.wire(std::format("s{} C>S mux{} {}B (unexpected mux): {}", session_, mux, payload.size(), hexdump(payload)));
        return;
    }
    const unsigned no = ++client_packets_;
    std::string names;
    for (const auto & b : mock_jtagd::split_blocks(payload))
        names += std::format(" {}", mock_jtagd::to_string(static_cast<Cmd>(b.code)));
    sinks_.wire(std::format("s{} C>S p{} {}B:{}", session_, no, payload.size(), names));
    pending_.emplace_back(no, std::move(payload));
}

void Decoder::server_packet(unsigned mux, std::vector<std::uint8_t> payload)
{
    if (mux >= mock_jtagd::fifo_mux_base) {
        auto & q = rx_[mux - mock_jtagd::fifo_mux_base];
        q.insert(q.end(), payload.begin(), payload.end());
        return;
    }
    if (mux == mock_jtagd::mux_notify) {
        sinks_.wire(std::format("s{} S>C notify {}B: {}", session_, payload.size(), hexdump(payload)));
        return;
    }
    if (mux != mock_jtagd::mux_command) {
        sinks_.wire(std::format("s{} S>C mux{} {}B (unexpected mux): {}", session_, mux, payload.size(), hexdump(payload)));
        return;
    }
    if (!greeted_) {
        Reader r(payload);
        const std::string sig = r.str();
        greeting_version_ = version_ = r.u32();
        const std::uint32_t auth = r.u32();
        greeted_ = true;
        sinks_.wire(std::format("s{} S>C greeting: version {}, auth 0x{:X}{}", session_, greeting_version_, auth,
                                sig == mock_jtagd::signature ? "" : " (unexpected signature)"));
        return;
    }
    if (pending_.empty())
        throw mock_jtagd::ProtocolError("response packet with no outstanding command packet");
    auto [no, cmd_payload] = std::move(pending_.front());
    pending_.pop_front();
    const auto cmds = mock_jtagd::split_blocks(cmd_payload);
    const auto resps = mock_jtagd::split_blocks(payload);

    std::string codes;
    for (const auto & b : resps)
        codes += std::format(" {}", b.code);
    sinks_.wire(std::format("s{} S>C p{} {} of {} responses:{}", session_, no, resps.size(), cmds.size(), codes));
    if (resps.size() > cmds.size())
        throw mock_jtagd::ProtocolError(std::format("p{}: {} responses to {} commands", no, resps.size(), cmds.size()));

    for (std::size_t i = 0; i < cmds.size(); ++i) {
        Op op;
        op.cmd = static_cast<Cmd>(cmds[i].code);
        op.args.assign(cmds[i].body.begin(), cmds[i].body.end());
        op.packet = no;
        if (i < resps.size()) {
            op.executed = true;
            op.err = static_cast<Err>(resps[i].code);
            op.resp.assign(resps[i].body.begin(), resps[i].body.end());
        }
        ops_.push_back(std::move(op));
    }
}

// Commands are processed strictly in order, each once all its FIFO bytes
// (sent by the client, or by the server in reply) have arrived.
void Decoder::pump()
{
    while (!ops_.empty()) {
        const Needs n = needs(ops_.front());
        if (tx_[0].size() < n.tx0 || tx_[1].size() < n.tx1 || rx_[0].size() < n.rx0)
            return;
        Op op = std::move(ops_.front());
        ops_.pop_front();
        auto t0 = take(tx_[0], n.tx0);
        auto t1 = take(tx_[1], n.tx1);
        auto r0 = take(rx_[0], n.rx0);
        process(op, std::move(t0), std::move(t1), std::move(r0));
    }
}

Decoder::Needs Decoder::needs(const Op & op) const
{
    Needs n;
    Reader a(op.args);
    Reader r(op.resp);
    switch (op.cmd) {
    case Cmd::access_ir_fifo: {
        a.u32();
        const std::uint32_t flags = a.u32();
        const std::uint32_t len = a.u32();
        n.tx0 = bytes_for(len);
        if (op.ok() && (flags & 1))
            n.rx0 = bytes_for(len);
        break;
    }
    case Cmd::access_dr: {
        a.u32();
        a.u32();
        a.u32();
        a.u32();
        const std::uint32_t w_len = a.u32();
        a.u32();
        const std::uint32_t r_len = a.u32();
        const std::uint32_t batch = version_ >= 5 ? a.u32() : 1;
        n.tx0 = bytes_for(w_len) * batch;
        if (op.ok())
            n.rx0 = bytes_for(r_len) * batch;
        break;
    }
    case Cmd::any_sequence: {
        a.u32();
        const std::uint32_t tcks = a.u32();
        const bool want = a.u32() != 0;
        n.tx0 = n.tx1 = bytes_for(tcks);
        if (op.ok() && want)
            n.rx0 = bytes_for(tcks);
        break;
    }
    case Cmd::get_hardware:
    case Cmd::get_quartus_devices:
        if (op.ok()) {
            r.u32();
            n.rx0 = r.u32();
        }
        break;
    case Cmd::read_chain:
    case Cmd::scan_chain:
    case Cmd::get_defined_devices:
        if (op.ok()) {
            r.u32();
            r.u32();
            n.rx0 = r.u32();
        }
        break;
    default:
        break;
    }
    return n;
}

// ---------------------------------------------------------------- semantics

bool Decoder::hub_capable(const TapInfo & t) const
{
    // Servers before v5 don't report the flag; the client then assumes it.
    return version_ < 5 || (t.features & mock_jtagd::devfeat_possible_hub);
}

std::string Decoder::node_name(std::uint32_t idcode, std::uint32_t select) const
{
    if (names_ && idcode != 0)
        for (const auto & c : names_->cables)
            for (const auto & t : c.taps)
                if (t.hub)
                    for (const auto & n : t.hub->nodes)
                        if (n.idcode == idcode)
                            return n.name;
    return std::format("node{}", select);
}

ScanRecord Decoder::base_record(const Op & op, const Open & open) const
{
    ScanRecord rec;
    rec.session = session_;
    rec.t_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_).count();
    rec.chain_id = open.chain;
    rec.result = op.err;
    rec.executed = op.executed;
    if (auto it = chains_.find(open.chain); it != chains_.end()) {
        rec.cable = it->second.cable;
        if (open.tap && *open.tap < it->second.taps.size()) {
            rec.device = it->second.taps[*open.tap].name;
            rec.device_idcode = it->second.taps[*open.tap].idcode;
        }
    }
    rec.tap = open.tap;
    return rec;
}

void Decoder::hub_user1(TapState & ts, std::uint32_t length, const BitWindow & write)
{
    Bits reg(length);
    reg.splice(write.offset, Bits::from_bytes(write.bytes, write.bits));
    const std::uint32_t r = ts.hub.idcode ? ts.hub.select_bits() + ts.hub.ir_bits() : length;
    // After the scan the register holds the last r bits shifted in.
    const std::uint64_t v = reg.slice(length >= r ? length - r : 0, std::min(r, length)).to_u64();
    if (ts.hub.idcode) {
        ts.node_ir = static_cast<std::uint32_t>(v & ((std::uint64_t{1} << ts.hub.ir_bits()) - 1));
        ts.select = static_cast<std::uint32_t>(v >> ts.hub.ir_bits());
        ts.select_known = true;
    } else {
        ts.select_known = (v == 0);
        ts.select = ts.node_ir = 0;
    }
    if (ts.select_known && ts.select == 0 && ts.node_ir == 0)
        ts.hub.nibbles.clear(); // HUB_INFO resets the ROM pointer
}

void Decoder::hub_user0_info(TapState & ts, const BitWindow & read)
{
    if (read.bits < 4 || read.bytes.empty())
        return;
    HubInfo & h = ts.hub;
    h.nibbles.push_back(read.bytes[0] & 0xF);
    if (h.nibbles.size() % 8 != 0)
        return;
    std::uint32_t word = 0;
    for (std::size_t i = 0; i < 8; ++i)
        word |= h.nibbles[h.nibbles.size() - 8 + i] << (4 * i);
    const std::size_t index = h.nibbles.size() / 8 - 1;
    if (index == 0) {
        h.idcode = word;
        h.nodes.assign(h.node_count(), 0);
    } else if (index - 1 < h.nodes.size()) {
        h.nodes[index - 1] = word;
    }
}

void Decoder::dr_scans(const Op & op, const std::vector<std::uint8_t> & tx, const std::vector<std::uint8_t> & rx)
{
    Reader a(op.args);
    const std::uint32_t open_id = a.u32();
    const std::uint32_t length = a.u32();
    a.u32(); // flags
    const std::uint32_t w_off = a.u32(), w_len = a.u32(), r_off = a.u32(), r_len = a.u32();
    const std::uint32_t batch = version_ >= 5 ? a.u32() : 1;
    const std::size_t wb = bytes_for(w_len), rb = bytes_for(r_len);

    const auto oit = opens_.find(open_id);
    const Open open = oit != opens_.end() ? oit->second : Open{};
    Chain * chain = chains_.contains(open.chain) ? &chains_[open.chain] : nullptr;
    TapState * ts = (chain && open.tap && *open.tap < chain->state.size()) ? &chain->state[*open.tap] : nullptr;
    const TapInfo * ti = (chain && open.tap && *open.tap < chain->taps.size()) ? &chain->taps[*open.tap] : nullptr;
    const bool hub = ts && ti && hub_capable(*ti) && ts->ir;

    for (std::uint32_t b = 0; b < batch; ++b) {
        ScanRecord rec = base_record(op, open);
        rec.op = ScanRecord::Op::dr;
        rec.length = length;
        if (w_len)
            rec.write = window(w_off, w_len, std::span(tx).subspan(std::min<std::size_t>(b * wb, tx.size())));
        if (r_len && !rx.empty())
            rec.read = window(r_off, r_len, std::span(rx).subspan(std::min<std::size_t>(b * rb, rx.size())));
        if (ts)
            rec.ir = ts->ir;

        if (hub && *ts->ir == mock_jtagd::user0_instruction && ts->select_known && ts->select > 0) {
            rec.node_select = ts->select;
            rec.node_ir = ts->node_ir;
            if (ts->select <= ts->hub.nodes.size())
                rec.node_idcode = ts->hub.nodes[ts->select - 1];
            rec.node = node_name(rec.node_idcode, ts->select);
        }
        if (op.ok() && hub) {
            if (*ts->ir == mock_jtagd::user1_instruction)
                hub_user1(*ts, length, rec.write);
            else if (*ts->ir == mock_jtagd::user0_instruction && ts->select_known && ts->select == 0 && ts->node_ir == 0)
                hub_user0_info(*ts, rec.read);
        }
        rec.seq = ++seq_;
        sinks_.scan(std::move(rec));
    }
}

void Decoder::process(const Op & op, std::vector<std::uint8_t> tx0, std::vector<std::uint8_t> tx1,
                      std::vector<std::uint8_t> rx0)
{
    sinks_.wire(std::format("s{} p{} {} [{}] -> {}{}{}{}{}", session_, op.packet, mock_jtagd::to_string(op.cmd),
                            hexdump(op.args), op.executed ? std::format("{}", static_cast<int>(op.err)) : "skipped",
                            op.resp.empty() ? "" : std::format(" [{}]", hexdump(op.resp)),
                            tx0.empty() ? "" : std::format(" tx[{}]", hexdump(tx0)),
                            tx1.empty() ? "" : std::format(" tx1[{}]", hexdump(tx1)),
                            rx0.empty() ? "" : std::format(" rx[{}]", hexdump(rx0))));

    Reader a(op.args);
    Reader r(op.resp);
    switch (op.cmd) {
    case Cmd::use_protocol_version:
        if (op.ok())
            version_ = std::min(a.u32(), greeting_version_);
        break;

    case Cmd::get_hardware:
        if (op.ok() && !rx0.empty()) {
            const std::uint32_t n = r.u32();
            Reader list(rx0);
            for (std::uint32_t i = 0; i < n; ++i) {
                const std::uint32_t id = list.u32();
                const std::string hw = list.str();
                const std::string port = list.str();
                list.u32(); // chain type
                list.str(); // device name
                if (version_ >= 2)
                    list.u32(); // features
                chains_[id].cable = port.empty() ? hw : std::format("{} [{}]", hw, port);
            }
        }
        break;

    case Cmd::read_chain:
    case Cmd::scan_chain:
        if (op.ok()) {
            const std::uint32_t chain_id = a.u32();
            r.u32(); // tag
            const std::uint32_t n = r.u32();
            Chain & c = chains_[chain_id];
            if (n == 0) {
                c.taps.clear();
                c.state.clear();
            } else if (!rx0.empty()) {
                Reader list(rx0);
                c.taps.clear();
                for (std::uint32_t i = 0; i < n; ++i) {
                    TapInfo t;
                    t.idcode = list.u32();
                    t.ir_length = list.u32();
                    t.features = list.u32();
                    list.u32();
                    list.u32();
                    t.name = list.str();
                    c.taps.push_back(std::move(t));
                }
                c.state.assign(c.taps.size(), TapState{});
            }
        }
        break;

    case Cmd::open_device:
        if (op.ok()) {
            const std::uint32_t chain = a.u32();
            const std::uint32_t tap = a.u32();
            const std::uint32_t claims = a.u32();
            if (version_ >= 13)
                for (std::uint32_t i = 0; i < claims; ++i) { a.u32(); a.u32(); a.u64(); }
            else if (version_ >= 2)
                for (std::uint32_t i = 0; i < claims; ++i) { a.u32(); a.u32(); }
            else {
                a.u32();
                for (std::uint32_t i = 0; i < claims; ++i) a.u32();
            }
            opens_[r.u32()] = Open{chain, tap, a.str()};
        }
        break;

    case Cmd::open_entire_chain:
        if (op.ok()) {
            const std::uint32_t chain = a.u32();
            a.u32();
            opens_[r.u32()] = Open{chain, std::nullopt, a.str()};
        }
        break;

    case Cmd::close_device:
        if (op.ok())
            opens_.erase(a.u32());
        break;

    case Cmd::test_logic_reset:
        if (op.ok())
            if (auto it = opens_.find(a.u32()); it != opens_.end())
                for (TapState & ts : chains_[it->second.chain].state)
                    ts.ir.reset();
        break;

    case Cmd::access_ir: {
        const std::uint32_t open_id = a.u32();
        const std::uint32_t instruction = a.u32();
        const Open open = opens_.contains(open_id) ? opens_[open_id] : Open{};
        ScanRecord rec = base_record(op, open);
        rec.op = ScanRecord::Op::ir;
        rec.ir = instruction;
        if (op.ok()) {
            rec.ir_capture = r.u32();
            if (open.tap && chains_[open.chain].state.size() > *open.tap)
                chains_[open.chain].state[*open.tap].ir = instruction;
        }
        rec.seq = ++seq_;
        sinks_.scan(std::move(rec));
        break;
    }

    case Cmd::access_ir_fifo: {
        const std::uint32_t open_id = a.u32();
        a.u32();
        const std::uint32_t length = a.u32();
        const Open open = opens_.contains(open_id) ? opens_[open_id] : Open{};
        ScanRecord rec = base_record(op, open);
        rec.op = ScanRecord::Op::ir;
        rec.length = length;
        rec.write = window(0, length, tx0);
        if (!rx0.empty())
            rec.read = window(0, length, rx0);
        if (op.ok()) {
            Chain & c = chains_[open.chain];
            const Bits bits = Bits::from_bytes(tx0, length);
            if (open.tap) {
                rec.ir = static_cast<std::uint32_t>(bits.to_u64());
                if (*open.tap < c.state.size())
                    c.state[*open.tap].ir = rec.ir;
            } else {
                // Whole chain: TAP 0 (nearest TDO) takes the first irlen bits.
                std::uint32_t offset = 0;
                for (std::size_t i = 0; i < c.taps.size() && i < c.state.size(); ++i) {
                    c.state[i].ir = static_cast<std::uint32_t>(bits.slice(offset, c.taps[i].ir_length).to_u64());
                    offset += c.taps[i].ir_length;
                }
            }
        }
        rec.seq = ++seq_;
        sinks_.scan(std::move(rec));
        break;
    }

    case Cmd::access_dr:
        dr_scans(op, tx0, rx0);
        break;

    case Cmd::any_sequence: {
        const std::uint32_t open_id = a.u32();
        const std::uint32_t tcks = a.u32();
        const Open open = opens_.contains(open_id) ? opens_[open_id] : Open{};
        ScanRecord rec = base_record(op, open);
        rec.op = ScanRecord::Op::any_sequence;
        rec.length = tcks;
        rec.write = window(0, tcks, tx0); // TDI; TMS is in the wire log
        if (!rx0.empty())
            rec.read = window(0, tcks, rx0);
        rec.seq = ++seq_;
        sinks_.scan(std::move(rec));
        break;
    }

    default:
        break;
    }
}

} // namespace jtag_proxy
