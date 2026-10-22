// jtag-proxy between the real client and the mock (standing in for a real
// jtagd): forwarding is transparent, scans are logged logically, and a
// capture turns back into the transcript that produced it.
#include "fixture.hpp"
#include "jtag_proxy/transcript.hpp"
#include "toy_sdm_client.hpp"

#include <ranges>

namespace roundtrip {
namespace {

constexpr DWORD fpga_tap = 1;
constexpr DWORD fpga_user_ir = 0x055;
constexpr std::uint32_t sdm_idcode = sld_node_idcode(1, toy::sdm_node_id, mfg_altera, 0);

using jtag_proxy::ScanRecord;

std::vector<ScanRecord> node_scans(const std::vector<ScanRecord> & all, std::uint32_t node_idcode)
{
    std::vector<ScanRecord> out;
    for (const auto & s : all)
        if (s.node_select && s.node_idcode == node_idcode)
            out.push_back(s);
    return out;
}

TEST_F(RoundTrip, ProxyIsTransparentAndLogsNodeScans)
{
    start_proxied("sdm_hello.yaml");
    toy::SdmClient sdm;
    ASSERT_EQ(AJI_NO_ERROR, sdm.find());
    ASSERT_EQ(AJI_NO_ERROR, sdm.open());
    toy::Response r;
    ASSERT_EQ(AJI_NO_ERROR, sdm.call(toy::get_idcode, {}, r));
    EXPECT_EQ(0x4341A0DDu, r.data.at(0));

    // The proxy learned the hub table from the client's own enumeration and
    // attributes the command/response scans to the sdm node.
    const auto scans = node_scans(proxy_->scans(), sdm_idcode);
    ASSERT_EQ(3u, scans.size());
    EXPECT_EQ("sdm", scans[0].node);
    EXPECT_EQ(std::optional<std::uint32_t>{toy::command_ir}, scans[0].node_ir);
    EXPECT_EQ("00000010", scans[0].write.hex());
    EXPECT_EQ(std::optional<std::uint32_t>{toy::response_ir}, scans[1].node_ir);
    EXPECT_EQ("00001000", scans[1].read.hex());
    EXPECT_EQ("4341A0DD", scans[2].read.hex());
    EXPECT_EQ("AGFB014R24A", scans[0].device);
    EXPECT_EQ("USB-BlasterII [1-3]", scans[0].cable);
}

TEST_F(RoundTrip, ProxyNamesNodesWithoutAScenario)
{
    start_proxied("sdm_hello.yaml", /*names=*/false);
    toy::SdmClient sdm;
    ASSERT_EQ(AJI_NO_ERROR, sdm.find());
    ASSERT_EQ(AJI_NO_ERROR, sdm.open());
    toy::Response r;
    ASSERT_EQ(AJI_NO_ERROR, sdm.call(toy::get_idcode, {}, r));
    const auto scans = node_scans(proxy_->scans(), sdm_idcode);
    ASSERT_FALSE(scans.empty());
    EXPECT_EQ("node2", scans[0].node);
}

TEST_F(RoundTrip, CaptureTurnsBackIntoTheTranscript)
{
    // Closing the loop: whatever transcript the "hardware" answered from, the
    // capture reproduces it, so replaying a capture of real hardware gives the
    // client what the hardware gave it.
    start_proxied("sdm_hello.yaml");
    toy::SdmClient sdm;
    ASSERT_EQ(AJI_NO_ERROR, sdm.find());
    ASSERT_EQ(AJI_NO_ERROR, sdm.open());
    toy::Response r;
    ASSERT_EQ(AJI_NO_ERROR, sdm.call(toy::get_idcode, {}, r));
    ASSERT_EQ(AJI_NO_ERROR, sdm.call(toy::get_idcode, {}, r)); // duplicates collapse

    jtag_proxy::ChannelSelector sel;
    sel.node_idcode = sdm_idcode;
    sel.command_ir = toy::command_ir;
    sel.response_ir = toy::response_ir;
    const auto result = jtag_proxy::extract_transcript(proxy_->scans(), sel);
    EXPECT_TRUE(result.warnings.empty());

    const Scenario original = load_scenario(std::filesystem::path(MOCK_JTAGD_SCENARIO_DIR) / "sdm_hello.yaml");
    const auto & expected = original.cables[0].taps[0].hub->nodes[1].channels[0].transcript;
    ASSERT_EQ(expected.size(), result.exchanges.size());
    EXPECT_EQ(expected[0].request, result.exchanges[0].request);
    EXPECT_EQ(expected[0].response, result.exchanges[0].response);

    // And the YAML snippet loads as part of a scenario.
    const std::string yaml = jtag_proxy::transcript_yaml(result, sel, "captured", "test");
    std::string scenario_text = "cables:\n  - chain:\n      - idcode: 1\n        irlen: 10\n        hub:\n"
                                "          ir_bits: 4\n          nodes:\n            - idcode: 0x0BD06E00\n";
    for (const auto & line : yaml | std::views::split('\n')) {
        const std::string l(line.begin(), line.end());
        if (!l.empty() && !l.starts_with('#'))
                scenario_text += "              " + l + "\n";
    }
    const Scenario reloaded = parse_scenario(scenario_text, "captured.yaml");
    const auto & ch = reloaded.cables[0].taps[0].hub->nodes[0].channels.at(0);
    EXPECT_EQ("observed", ch.provenance);
    EXPECT_EQ(expected[0].response, ch.transcript.at(0).response);
}

// The point of logging scans rather than packets: the same logical operations
// sent unbatched and batched produce different wire traffic but the same scan log.
TEST_F(RoundTrip, BatchingDoesNotChangeTheScanLog)
{
    start_proxied(Scenario::default_board());
    AJI_OPEN_ID open = open_locked(fpga_tap, {{AJI_CLAIM_IR_SHARED, 0, fpga_user_ir}}, AJI_PACK_NEVER);

    auto run = [&] {
        const BYTE a[4] = {0x11, 0x22, 0x33, 0x44};
        const BYTE b[3][2] = {{1, 2}, {3, 4}, {5, 6}};
        BYTE rd[4] = {}, rd3[3][2] = {};
        ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, fpga_user_ir, nullptr));
        ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 32, 0, 0, 32, a, 0, 0, nullptr));
        ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 32, 0, 0, 0, nullptr, 0, 32, rd));
        ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 16, 0, 0, 16, &b[0][0], 0, 16, &rd3[0][0], 3));
        ASSERT_EQ(AJI_NO_ERROR, aji_flush(open));
    };
    auto packets_so_far = [&] {
        return std::ranges::count_if(proxy_->wire_log(), [](const std::string & l) { return l.find(" C>S p") != std::string::npos; });
    };

    run();
    const auto first = proxy_->scans();
    const auto packets_unbatched = packets_so_far();
    ASSERT_EQ(AJI_NO_ERROR, aji_unlock(open));
    ASSERT_EQ(AJI_NO_ERROR, aji_lock(open, 1000, AJI_PACK_MANUAL));
    const auto packets_before = packets_so_far();
    run();
    const auto all = proxy_->scans();
    const auto packets_batched = packets_so_far() - packets_before;

    auto logical = [](const ScanRecord & s) {
        return std::format("{} tap{} ir{:X} len{} w{} r{} {}", static_cast<int>(s.op), s.tap.value_or(99),
                           s.ir.value_or(0), s.length, s.write.hex(), s.read.hex(), static_cast<int>(s.result));
    };
    // Scans of the second run follow those of the first.
    const std::vector<ScanRecord> second(all.begin() + static_cast<std::ptrdiff_t>(first.size()), all.end());
    std::vector<std::string> a, b;
    for (const auto & s : first | std::views::drop(first.size() - second.size()))
        a.push_back(logical(s));
    for (const auto & s : second)
        b.push_back(logical(s));
    EXPECT_EQ(a, b);
    EXPECT_EQ(6u, second.size()) << "1 IR + 2 DR + 3 batched DR, each logged on its own";
    EXPECT_LT(packets_batched, packets_unbatched) << "the wire traffic did differ";
}

TEST_F(RoundTrip, ProxyWireLogShowsServerStoppingAtFirstError)
{
    start_proxied(Scenario::default_board());
    AJI_OPEN_ID open = open_locked(fpga_tap, {{AJI_CLAIM_IR_SHARED, 0, fpga_user_ir}}, AJI_PACK_MANUAL);
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, fpga_user_ir, nullptr));
    ASSERT_EQ(AJI_NO_ERROR, aji_flush(open));
    server_->inject({.cmd = Cmd::access_dr, .error = Err::failure, .skip = 1});
    const BYTE w[4] = {1, 2, 3, 4};
    for (int i = 0; i < 3; ++i)
        ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 32, 0, 0, 32, w, 0, 0, nullptr));
    EXPECT_EQ(AJI_FAILURE, aji_flush(open));

    const auto log = proxy_->wire_log();
    EXPECT_TRUE(std::ranges::any_of(log, [](const std::string & l) {
        return l.find("2 of 3 responses: 0 1") != std::string::npos;
    })) << "the reply to the 3-command batch carries 2 responses";
    const auto scans = proxy_->scans();
    ASSERT_GE(scans.size(), 3u);
    EXPECT_EQ(Err::failure, scans[scans.size() - 2].result);
    EXPECT_FALSE(scans.back().executed);
}

TEST_F(RoundTrip, ScanLogFileRoundTrips)
{
    const auto log = std::filesystem::path(::testing::TempDir()) / std::format("scan_{}.yaml", ::getpid());
    Scenario s = load_scenario(std::filesystem::path(MOCK_JTAGD_SCENARIO_DIR) / "sdm_hello.yaml");
    server_ = std::make_unique<Server>(s);
    jtag_proxy::ProxyOptions po;
    po.upstream_port = server_->port();
    po.scan_log = log;
    po.names = s;
    proxy_ = std::make_unique<jtag_proxy::Proxy>(std::move(po));
    config_path_ = std::filesystem::path(::testing::TempDir()) / std::format("aji_client_{}.conf", ::getpid());
    std::ofstream(config_path_) << std::format("ReplaceLocalJtagServer = \"127.0.0.1:{}\";\n", proxy_->port());
    ::setenv("QUARTUS_JTAG_CLIENT_CONFIG", config_path_.c_str(), 1);

    toy::SdmClient sdm;
    ASSERT_EQ(AJI_NO_ERROR, sdm.find());
    ASSERT_EQ(AJI_NO_ERROR, sdm.open());
    toy::Response r;
    ASSERT_EQ(AJI_NO_ERROR, sdm.call(toy::get_idcode, {}, r));

    const auto in_memory = proxy_->scans();
    const auto from_file = jtag_proxy::load_scan_log(log);
    ASSERT_EQ(in_memory.size(), from_file.size());
    for (std::size_t i = 0; i < in_memory.size(); ++i)
        EXPECT_EQ(in_memory[i].to_yaml(), from_file[i].to_yaml());
    std::filesystem::remove(log);
}

TEST(BitWindow, HexAndWords)
{
    const auto w = jtag_proxy::BitWindow::from_hex(0, 64, "4341A0DD_00001000");
    EXPECT_EQ("4341A0DD_00001000", w.hex());
    EXPECT_EQ((std::vector<std::uint64_t>{0x00001000, 0x4341A0DD}), w.words(32));
    const auto odd = jtag_proxy::BitWindow::from_hex(3, 9, "1A5");
    EXPECT_EQ("1A5", odd.hex());
    EXPECT_EQ(3u, odd.offset);
}

} // namespace
} // namespace roundtrip
