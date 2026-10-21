#include "mock_jtagd/scenario_yaml.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <initializer_list>
#include <sstream>

namespace mock_jtagd {
namespace {

class Loader {
public:
    explicit Loader(std::string origin) : origin_(std::move(origin)) {}

    Scenario scenario(const YAML::Node & root);

private:
    [[noreturn]] void fail(const YAML::Node & at, std::string_view what) const
    {
        const YAML::Mark m = at.Mark();
        if (m.is_null())
            throw ScenarioError(std::format("{}: {}", origin_, what));
        throw ScenarioError(std::format("{}:{}:{}: {}", origin_, m.line + 1, m.column + 1, what));
    }

    // Reject keys outside `allowed`, so a typo can't silently mean "default".
    void expect_map(const YAML::Node & n, std::string_view what, std::initializer_list<std::string_view> allowed) const
    {
        if (!n.IsMap())
            fail(n, std::format("{} must be a mapping", what));
        for (const auto & kv : n) {
            const auto key = kv.first.as<std::string>();
            if (std::ranges::find(allowed, key) == allowed.end()) {
                std::string list;
                for (auto a : allowed)
                    list += std::format("{}{}", list.empty() ? "" : ", ", a);
                fail(kv.first, std::format("unknown key '{}' in {} (expected one of: {})", key, what, list));
            }
        }
    }

    std::uint64_t integer(const YAML::Node & n, std::string_view what, std::uint64_t max = ~0ull) const
    {
        if (!n.IsScalar())
            fail(n, std::format("{} must be an integer", what));
        std::string text = n.Scalar();
        std::erase(text, '_');
        int base = 10;
        std::string_view digits = text;
        if (digits.starts_with("0x") || digits.starts_with("0X")) {
            base = 16;
            digits.remove_prefix(2);
        } else if (digits.starts_with("0b") || digits.starts_with("0B")) {
            base = 2;
            digits.remove_prefix(2);
        }
        std::uint64_t v = 0;
        const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v, base);
        if (digits.empty() || ec != std::errc{} || end != digits.data() + digits.size())
            fail(n, std::format("{}: '{}' is not an integer (decimal, 0x.. or 0b..)", what, n.Scalar()));
        if (v > max)
            fail(n, std::format("{}: {} is out of range (max 0x{:X})", what, n.Scalar(), max));
        return v;
    }

    std::uint32_t u32(const YAML::Node & n, std::string_view what, std::uint64_t max = 0xFFFF'FFFF) const
    {
        return static_cast<std::uint32_t>(integer(n, what, max));
    }

    std::string string(const YAML::Node & n, std::string_view what) const
    {
        if (!n.IsScalar())
            fail(n, std::format("{} must be a string", what));
        return n.Scalar();
    }

    std::vector<std::uint64_t> words(const YAML::Node & n, std::string_view what, std::uint32_t bits) const
    {
        if (!n.IsSequence())
            fail(n, std::format("{} must be a list of words", what));
        std::vector<std::uint64_t> out;
        for (const auto & w : n)
            out.push_back(integer(w, what, bits >= 64 ? ~0ull : (1ull << bits) - 1));
        return out;
    }

    std::string provenance(const YAML::Node & n) const
    {
        if (!n)
            return {};
        const std::string p = string(n, "provenance");
        if (p != "observed" && p != "documented" && p != "inferred" && p != "placeholder")
            fail(n, std::format("provenance '{}' must be observed, documented, inferred or placeholder", p));
        return p;
    }

    std::uint32_t node_idcode(const YAML::Node & n) const
    {
        if (n.IsScalar())
            return u32(n, "node idcode");
        expect_map(n, "node idcode", {"version", "node", "mfg", "instance"});
        if (!n["node"])
            fail(n, "node idcode needs 'node' (the 8-bit node id)");
        return sld_node_idcode(n["version"] ? u32(n["version"], "version", 0x1F) : 1, u32(n["node"], "node", 0xFF),
                               n["mfg"] ? u32(n["mfg"], "mfg", 0x7FF) : mfg_altera,
                               n["instance"] ? u32(n["instance"], "instance", 0xFF) : 0);
    }

    std::uint32_t features(const YAML::Node & n) const
    {
        if (n.IsScalar())
            return u32(n, "features");
        if (!n.IsSequence())
            fail(n, "features must be an integer or a list of names");
        std::uint32_t f = 0;
        for (const auto & item : n) {
            const std::string name = string(item, "feature");
            if (name == "possible_hub") f |= devfeat_possible_hub;
            else if (name == "bad_ir_capture") f |= devfeat_bad_ir_capture;
            else if (name == "user01_break_ir") f |= devfeat_user01_break_ir;
            else if (name == "command_response_chain") f |= devfeat_command_response_chain;
            else fail(item, std::format("unknown feature '{}' (possible_hub, bad_ir_capture, "
                                        "user01_break_ir, command_response_chain)", name));
        }
        return f;
    }

    RegisterSpec register_spec(const YAML::Node & n, std::string_view where) const
    {
        if (n.IsScalar()) { // shorthand: "0x055: rw"
            YAML::Node m;
            m["kind"] = n;
            return register_spec_map(n, m, where);
        }
        return register_spec_map(n, n, where);
    }

    RegisterSpec register_spec_map(const YAML::Node & at, const YAML::Node & n, std::string_view where) const
    {
        expect_map(n, std::format("register {}", where), {"kind", "length", "value", "values", "provenance"});
        RegisterSpec r;
        const std::string kind = n["kind"] ? string(n["kind"], "kind") : "rw";
        if (kind == "rw") r.kind = RegisterSpec::Kind::rw;
        else if (kind == "const") r.kind = RegisterSpec::Kind::constant;
        else if (kind == "sequence") r.kind = RegisterSpec::Kind::sequence;
        else fail(n["kind"] ? n["kind"] : at, std::format("register {}: kind '{}' must be rw, const or sequence", where, kind));

        if (n["length"])
            r.length = u32(n["length"], "length", 64);
        if (r.kind != RegisterSpec::Kind::rw && r.length == 0)
            fail(at, std::format("register {}: {} registers need a length (1..64)", where, kind));
        const std::uint64_t max = (r.length == 0 || r.length >= 64) ? ~0ull : (1ull << r.length) - 1;
        if (n["value"])
            r.value = integer(n["value"], "value", max);
        if (r.kind == RegisterSpec::Kind::constant && !n["value"])
            fail(at, std::format("register {}: const registers need a value", where));
        if (n["values"]) {
            if (r.kind != RegisterSpec::Kind::sequence)
                fail(n["values"], "'values' only applies to sequence registers");
            r.values = words(n["values"], "values", r.length);
        }
        if (r.kind == RegisterSpec::Kind::sequence && r.values.empty())
            fail(at, std::format("register {}: sequence registers need a non-empty 'values' list", where));
        r.provenance = provenance(n["provenance"]);
        return r;
    }

    std::map<std::uint32_t, RegisterSpec> registers(const YAML::Node & n, std::string_view owner,
                                                     std::uint32_t ir_bits) const
    {
        if (!n.IsMap())
            fail(n, std::format("{}: registers must map IR values to register specs", owner));
        std::map<std::uint32_t, RegisterSpec> out;
        for (const auto & kv : n) {
            const std::uint32_t ir = u32(kv.first, "IR value", (1ull << ir_bits) - 1);
            if (!out.emplace(ir, register_spec(kv.second, std::format("0x{:X} of {}", ir, owner))).second)
                fail(kv.first, std::format("{}: IR 0x{:X} listed twice", owner, ir));
        }
        return out;
    }

    // Parse a `channels:` list for `owner` ("node 'x'" / "device 'x'"), rejecting
    // IRs already taken by registers, other channels or `reserved`.
    std::vector<Channel> channel_list(const YAML::Node & list, std::string_view owner, std::uint32_t ir_bits,
                                      const std::map<std::uint32_t, RegisterSpec> & regs,
                                      std::initializer_list<std::uint32_t> reserved = {}) const
    {
        if (!list.IsSequence())
            fail(list, "channels must be a list");
        std::vector<Channel> out;
        for (const auto & c : list) {
            Channel ch = channel(c, owner, ir_bits);
            for (std::uint32_t ir : {ch.command_ir, ch.response_ir}) {
                if (regs.contains(ir))
                    fail(c, std::format("channel '{}' IR 0x{:X} is also declared as a register", ch.name, ir));
                if (std::ranges::find(reserved, ir) != reserved.end())
                    fail(c, std::format("channel '{}' IR 0x{:X} is reserved (IDCODE, BYPASS or hub USER0/USER1)",
                                        ch.name, ir));
                for (const Channel & other : out)
                    if (ir == other.command_ir || ir == other.response_ir)
                        fail(c, std::format("channel '{}' IR 0x{:X} is already used by channel '{}'", ch.name, ir,
                                            other.name));
            }
            out.push_back(std::move(ch));
        }
        return out;
    }

    Channel channel(const YAML::Node & n, std::string_view owner, std::uint32_t ir_bits) const
    {
        expect_map(n, "channel",
                   {"name", "command_ir", "response_ir", "word_bits", "idle", "transcript", "provenance"});
        Channel c;
        c.name = n["name"] ? string(n["name"], "name") : "channel";
        if (!n["command_ir"] || !n["response_ir"])
            fail(n, std::format("channel '{}' on {} needs command_ir and response_ir", c.name, owner));
        const std::uint64_t ir_max = (1ull << ir_bits) - 1;
        c.command_ir = u32(n["command_ir"], "command_ir", ir_max);
        c.response_ir = u32(n["response_ir"], "response_ir", ir_max);
        if (c.command_ir == c.response_ir)
            fail(n["response_ir"], "command_ir and response_ir must differ");
        if (n["word_bits"])
            c.word_bits = u32(n["word_bits"], "word_bits", 64);
        if (c.word_bits == 0)
            fail(n["word_bits"], "word_bits must be 1..64");
        const std::uint64_t word_max = c.word_bits >= 64 ? ~0ull : (1ull << c.word_bits) - 1;
        if (n["idle"])
            c.idle = integer(n["idle"], "idle", word_max);
        if (n["transcript"]) {
            if (!n["transcript"].IsSequence())
                fail(n["transcript"], "transcript must be a list of {request, response}");
            for (const auto & ex : n["transcript"]) {
                expect_map(ex, "transcript entry", {"request", "response"});
                if (!ex["request"] || !ex["response"])
                    fail(ex, "transcript entry needs request and response");
                Exchange e{words(ex["request"], "request", c.word_bits), words(ex["response"], "response", c.word_bits)};
                if (e.request.empty())
                    fail(ex["request"], "request must not be empty");
                c.transcript.push_back(std::move(e));
            }
        }
        c.provenance = provenance(n["provenance"]);
        return c;
    }

    SldNode node(const YAML::Node & n, std::size_t index, std::uint32_t ir_bits) const
    {
        expect_map(n, "hub node", {"name", "idcode", "registers", "channels", "provenance", "hub"});
        if (n["hub"])
            fail(n["hub"], "nested hubs (bridge nodes) are not supported by the mock yet");
        SldNode out;
        out.name = n["name"] ? string(n["name"], "name") : std::format("node{}", index);
        if (!n["idcode"])
            fail(n, std::format("node '{}' needs an idcode", out.name));
        out.idcode = node_idcode(n["idcode"]);
        if (n["registers"])
            out.registers = registers(n["registers"], std::format("node '{}'", out.name), ir_bits);
        if (n["channels"])
            out.channels = channel_list(n["channels"], std::format("node '{}'", out.name), ir_bits, out.registers);
        out.provenance = provenance(n["provenance"]);
        return out;
    }

    SldHub hub(const YAML::Node & n) const
    {
        expect_map(n, "hub", {"ir_bits", "version", "mfg", "nodes"});
        SldHub h;
        if (n["ir_bits"])
            h.ir_bits = u32(n["ir_bits"], "ir_bits", 32);
        if (h.ir_bits == 0)
            fail(n["ir_bits"], "ir_bits must be at least 1 (the client ignores hubs with 0)");
        if (n["version"])
            h.hub_version = u32(n["version"], "version", 0x1F);
        if (n["mfg"])
            h.mfg = u32(n["mfg"], "mfg", 0x7FF);
        if (n["nodes"]) {
            if (!n["nodes"].IsSequence())
                fail(n["nodes"], "nodes must be a list");
            for (const auto & node_yaml : n["nodes"]) {
                SldNode nd = node(node_yaml, h.nodes.size(), h.ir_bits);
                for (const SldNode & other : h.nodes)
                    if (other.name == nd.name)
                        fail(node_yaml, std::format("node name '{}' is used twice", nd.name));
                h.nodes.push_back(std::move(nd));
            }
        }
        if (h.nodes.size() > 255)
            fail(n["nodes"], "a hub holds at most 255 nodes");
        if (h.select_bits() + h.ir_bits > 64)
            fail(n, "select bits + ir_bits exceed 64; the client refuses such hubs");
        return h;
    }

    Tap tap(const YAML::Node & n) const
    {
        expect_map(n, "chain entry",
                   {"name", "idcode", "irlen", "idcode_ir", "ir_capture", "features", "registers", "channels", "hub",
                    "provenance"});
        Tap t;
        if (n["name"])
            t.name = string(n["name"], "name");
        if (!n["idcode"] || !n["irlen"])
            fail(n, std::format("device '{}' needs idcode and irlen", t.name));
        t.idcode = u32(n["idcode"], "idcode");
        t.ir_length = u32(n["irlen"], "irlen", 31);
        if (t.ir_length < 2)
            fail(n["irlen"], "irlen must be 2..31");
        const std::uint64_t ir_max = (1ull << t.ir_length) - 1;
        if (n["idcode_ir"])
            t.idcode_instruction = u32(n["idcode_ir"], "idcode_ir", ir_max);
        else if (t.idcode_instruction > ir_max)
            fail(n, std::format("device '{}': default idcode_ir 0x{:X} doesn't fit irlen {}; set idcode_ir",
                                t.name, t.idcode_instruction, t.ir_length));
        if (n["ir_capture"])
            t.ir_capture = u32(n["ir_capture"], "ir_capture", ir_max);
        if (n["features"])
            t.features = features(n["features"]);
        if (n["hub"]) {
            t.hub = hub(n["hub"]);
            t.features |= devfeat_possible_hub; // the client only looks for hubs on these
            if (t.ir_length < 4)
                fail(n["hub"], "a hub needs USER0/USER1 (0xC/0xE), so irlen must be at least 4");
        }
        if (n["registers"]) {
            t.registers = registers(n["registers"], std::format("device '{}'", t.name), t.ir_length);
            for (const auto & [ir, spec] : t.registers) {
                if (ir == t.idcode_instruction || ir == ir_max)
                    fail(n["registers"], std::format("device '{}': IR 0x{:X} is IDCODE/BYPASS and can't be redefined",
                                                     t.name, ir));
                if (t.hub && (ir == user0_instruction || ir == user1_instruction))
                    fail(n["registers"], std::format("device '{}': IR 0x{:X} belongs to the hub", t.name, ir));
            }
        }
        if (n["channels"]) {
            const std::uint32_t bypass = static_cast<std::uint32_t>(ir_max);
            t.channels = t.hub ? channel_list(n["channels"], std::format("device '{}'", t.name), t.ir_length,
                                              t.registers,
                                              {t.idcode_instruction, bypass, user0_instruction, user1_instruction})
                               : channel_list(n["channels"], std::format("device '{}'", t.name), t.ir_length,
                                              t.registers, {t.idcode_instruction, bypass});
        }
        t.provenance = provenance(n["provenance"]);
        return t;
    }

    Cable cable(const YAML::Node & n) const
    {
        expect_map(n, "cable", {"hw_name", "port", "device_name", "parameters", "chain"});
        Cable c;
        if (n["hw_name"])
            c.hw_name = string(n["hw_name"], "hw_name");
        if (n["port"])
            c.port = string(n["port"], "port");
        if (n["device_name"])
            c.device_name = string(n["device_name"], "device_name");
        if (n["parameters"]) {
            if (!n["parameters"].IsMap())
                fail(n["parameters"], "parameters must map names to integers");
            for (const auto & kv : n["parameters"])
                c.parameters[kv.first.as<std::string>()] = u32(kv.second, "parameter value");
        }
        if (n["chain"]) {
            if (!n["chain"].IsSequence())
                fail(n["chain"], "chain must be a list of devices, nearest TDO first");
            for (const auto & t : n["chain"])
                c.taps.push_back(tap(t));
        }
        return c;
    }

    std::string origin_;
};

Scenario Loader::scenario(const YAML::Node & root)
{
    if (!root || root.IsNull())
        return Scenario{};
    expect_map(root, "scenario",
               {"protocol_version", "password", "allow_remote", "version_info", "server_path", "cables"});
    Scenario s;
    if (root["protocol_version"])
        s.version = u32(root["protocol_version"], "protocol_version", current_version);
    if (s.version < 1)
        fail(root["protocol_version"], "protocol_version must be 1..13");
    if (root["password"] && !root["password"].IsNull())
        s.password = string(root["password"], "password");
    if (root["allow_remote"] && root["allow_remote"].as<bool>())
        s.server_flags |= 1;
    if (root["version_info"])
        s.version_info = string(root["version_info"], "version_info");
    if (root["server_path"])
        s.server_path = string(root["server_path"], "server_path");
    if (root["cables"]) {
        if (!root["cables"].IsSequence())
            fail(root["cables"], "cables must be a list");
        for (const auto & c : root["cables"])
            s.cables.push_back(cable(c));
    }
    return s;
}

Scenario parse(const std::string & text, const std::string & origin)
{
    YAML::Node root;
    try {
        root = YAML::Load(text);
    } catch (const YAML::Exception & e) {
        throw ScenarioError(std::format("{}:{}:{}: {}", origin, e.mark.line + 1, e.mark.column + 1, e.msg));
    }
    try {
        return Loader(origin).scenario(root);
    } catch (const YAML::Exception & e) { // e.g. a bad bool
        throw ScenarioError(std::format("{}:{}:{}: {}", origin, e.mark.line + 1, e.mark.column + 1, e.msg));
    }
}

std::string with_provenance(std::string_view p) { return p.empty() ? "" : std::format("  [{}]", p); }

} // namespace

Scenario load_scenario(const std::filesystem::path & file)
{
    std::ifstream in(file);
    if (!in)
        throw ScenarioError(std::format("{}: cannot open", file.string()));
    std::stringstream ss;
    ss << in.rdbuf();
    return parse(ss.str(), file.string());
}

Scenario parse_scenario(std::string_view yaml, std::string_view origin)
{
    return parse(std::string(yaml), std::string(origin));
}

std::string describe(const Scenario & s)
{
    std::string out = std::format("protocol v{}{}{}\n", s.version, s.password ? ", password required" : "",
                                  (s.server_flags & 1) ? ", remote clients allowed" : "");
    auto channel_line = [](const Channel & ch) {
        return std::format("      channel {}: command IR 0x{:X}, response IR 0x{:X}, {}-bit words, "
                           "{} transcript entries{}\n",
                           ch.name, ch.command_ir, ch.response_ir, ch.word_bits, ch.transcript.size(),
                           with_provenance(ch.provenance));
    };
    auto reg_line = [](std::uint32_t ir, const RegisterSpec & r) {
        std::string what;
        switch (r.kind) {
        case RegisterSpec::Kind::rw:
            what = r.length ? std::format("rw {} bits", r.length) : "rw";
            break;
        case RegisterSpec::Kind::constant:
            what = std::format("const {} bits = 0x{:X}", r.length, r.value);
            break;
        case RegisterSpec::Kind::sequence:
            what = std::format("sequence {} bits, {} values", r.length, r.values.size());
            break;
        }
        return std::format("IR 0x{:X}: {}{}", ir, what, with_provenance(r.provenance));
    };
    for (std::size_t i = 0; i < s.cables.size(); ++i) {
        const Cable & c = s.cables[i];
        out += std::format("{}) {}{}{}\n", i + 1, c.hw_name, c.port.empty() ? "" : std::format(" [{}]", c.port),
                           c.device_name.empty() ? "" : std::format(" \"{}\"", c.device_name));
        for (const Tap & t : c.taps) {
            out += std::format("  {:08X}   {:<12} irlen {}{}\n", t.idcode, t.name, t.ir_length,
                               with_provenance(t.provenance));
            for (const auto & [ir, r] : t.registers)
                out += std::format("      {}\n", reg_line(ir, r));
            for (const Channel & ch : t.channels)
                out += channel_line(ch);
            if (!t.hub)
                continue;
            for (const SldNode & n : t.hub->nodes) {
                out += std::format("    Node {:08X}  {:<12} (mfg 0x{:X} node 0x{:02X} v{} #{}){}\n", n.idcode, n.name,
                                   (n.idcode >> 8) & 0x7FF, (n.idcode >> 19) & 0xFF, n.idcode >> 27,
                                   n.idcode & 0xFF, with_provenance(n.provenance));
                for (const auto & [ir, r] : n.registers)
                    out += std::format("      {}\n", reg_line(ir, r));
                for (const Channel & ch : n.channels)
                    out += channel_line(ch);
            }
        }
    }
    return out;
}

} // namespace mock_jtagd
