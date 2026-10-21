// SLD hub discovery and node access, which the client implements on top of
// USER1/USER0 scans: the mock has to emulate the hub ROM for these to work.
#include "fixture.hpp"

namespace roundtrip {
namespace {

constexpr DWORD fpga_tap = 1;

TEST_F(RoundTrip, GetNodesReadsHubRom)
{
    start();
    AJI_CHAIN_ID chain = locked_chain();
    DWORD idcodes[8] = {};
    DWORD n = 8;
    DWORD hub_info = 0;
    ASSERT_EQ(AJI_NO_ERROR, aji_get_nodes(chain, fpga_tap, idcodes, &n, &hub_info));
    const Scenario scenario = Scenario::default_board();
    const SldHub & hub = *scenario.cables[0].taps[1].hub;
    ASSERT_EQ(hub.nodes.size(), n);
    EXPECT_EQ(hub.nodes[0].idcode, idcodes[0]);
    EXPECT_EQ(hub.nodes[1].idcode, idcodes[1]);
    EXPECT_EQ(hub.idcode(), hub_info);
}

TEST_F(RoundTrip, GetNodesHierarchicalApi)
{
    start();
    AJI_CHAIN_ID chain = locked_chain();
    AJI_HIER_ID ids[8] = {};
    AJI_HUB_INFO infos[8] = {};
    DWORD n = 8;
    ASSERT_EQ(AJI_NO_ERROR, aji_get_nodes(chain, fpga_tap, ids, &n, infos));
    ASSERT_EQ(2u, n);
    EXPECT_EQ(0u, ids[0].position_n);
    EXPECT_EQ(sld_node_idcode(1, 0x08, mfg_altera, 1), ids[1].idcode);
}

TEST_F(RoundTrip, TapWithoutHubHasNoNodes)
{
    start();
    AJI_CHAIN_ID chain = locked_chain();
    DWORD idcodes[8] = {};
    DWORD n = 8;
    ASSERT_EQ(AJI_NO_ERROR, aji_get_nodes(chain, 0, idcodes, &n));
    EXPECT_EQ(0u, n);
}

TEST_F(RoundTrip, ManyNodes)
{
    // README 0.4.0 notes a crash with >= 10 nodes; keep that covered.
    Scenario s = Scenario::default_board();
    SldHub & hub = *s.cables[0].taps[1].hub;
    hub.nodes.clear();
    for (std::uint32_t i = 0; i < 20; ++i)
        hub.nodes.push_back(sld_node_idcode(1, 0x08, mfg_altera, i));
    start(s);

    AJI_CHAIN_ID chain = locked_chain();
    DWORD idcodes[32] = {};
    DWORD n = 32;
    ASSERT_EQ(AJI_NO_ERROR, aji_get_nodes(chain, fpga_tap, idcodes, &n));
    ASSERT_EQ(20u, n);
    for (std::uint32_t i = 0; i < 20; ++i)
        EXPECT_EQ(hub.nodes[i].idcode, idcodes[i]);
}

TEST_F(RoundTrip, OpenNodeAndAccessItsRegisters)
{
    start();
    AJI_CHAIN_ID chain = locked_chain();
    const DWORD node_idcode = sld_node_idcode(1, 0x08, mfg_altera, 1);
    const AJI_CLAIM claims[] = {{AJI_CLAIM_OVERLAY, 1}, {AJI_CLAIM_OVERLAY, 2}};
    AJI_OPEN_ID node = nullptr;
    ASSERT_EQ(AJI_NO_ERROR, aji_open_node(chain, fpga_tap, node_idcode, &node, claims, 2, "roundtrip"));
    ASSERT_EQ(AJI_NO_ERROR, aji_unlock_chain_lock(chain, node, AJI_PACK_NEVER));

    // Select node IR 1, write a value; select IR 2, write another; read both back.
    const BYTE a[2] = {0x34, 0x12}, b[2] = {0xCD, 0xAB};
    BYTE rd[2] = {};
    ASSERT_EQ(AJI_NO_ERROR, aji_access_overlay(node, 1, nullptr));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(node, 16, 0, 0, 16, a, 0, 0, nullptr));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_overlay(node, 2, nullptr));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(node, 16, 0, 0, 16, b, 0, 0, nullptr));

    DWORD captured = ~0u;
    ASSERT_EQ(AJI_NO_ERROR, aji_access_overlay(node, 1, &captured));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(node, 16, 0, 0, 0, nullptr, 0, 16, rd));
    ASSERT_EQ(AJI_NO_ERROR, aji_flush(node));
    EXPECT_EQ(0x34, rd[0]);
    EXPECT_EQ(0x12, rd[1]);

    ASSERT_EQ(AJI_NO_ERROR, aji_access_overlay(node, 2, nullptr));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(node, 16, 0, 0, 0, nullptr, 0, 16, rd));
    EXPECT_EQ(0xCD, rd[0]);
    EXPECT_EQ(0xAB, rd[1]);
}

} // namespace
} // namespace roundtrip
