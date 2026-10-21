// Device-level JTAG operations: IR/DR scans, batching/packing, whole-chain access.
#include "fixture.hpp"

#include <numeric>

namespace roundtrip {
namespace {

constexpr DWORD fpga_tap = 1;
constexpr DWORD fpga_idcode_ir = 0x006;
constexpr DWORD fpga_bypass_ir = 0x3FF;
constexpr DWORD fpga_user_ir = 0x055; // no special meaning: a plain R/W data register in the mock

std::vector<AJI_CLAIM2> claims_for(std::initializer_list<DWORD> instructions)
{
    std::vector<AJI_CLAIM2> claims;
    for (DWORD ir : instructions)
        claims.push_back({AJI_CLAIM_IR_SHARED, 0, ir});
    return claims;
}

// ------------------------------------------------ every pack style

class PackStyle : public RoundTrip, public ::testing::WithParamInterface<AJI_PACK_STYLE> {};

TEST_P(PackStyle, ReadsIdcodeThroughDr)
{
    start();
    AJI_OPEN_ID open = open_locked(fpga_tap, claims_for({fpga_idcode_ir}), GetParam());

    DWORD captured = 0;
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, fpga_idcode_ir, &captured));
    BYTE idcode[4] = {};
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 32, 0, 0, 0, nullptr, 0, 32, idcode));
    ASSERT_EQ(AJI_NO_ERROR, aji_flush(open)); // deferred results only land here when packing
    EXPECT_EQ(1u, captured);
    EXPECT_EQ(0x02D120DDu, le32(idcode));

    EXPECT_EQ(AJI_NO_ERROR, aji_unlock(open));
    EXPECT_EQ(AJI_NO_ERROR, aji_close_device(open));
}

TEST_P(PackStyle, DataRegisterKeepsLastWrittenValue)
{
    start();
    AJI_OPEN_ID open = open_locked(fpga_tap, claims_for({fpga_user_ir}), GetParam());
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, fpga_user_ir, nullptr));

    // Batch of 4 scans: each reads back what the previous one wrote.
    BYTE wr[4][8], rd[4][8] = {};
    for (int b = 0; b < 4; ++b)
        for (int i = 0; i < 8; ++i)
            wr[b][i] = static_cast<BYTE>(0x10 * (b + 1) + i);
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 64, 0, 0, 64, &wr[0][0], 0, 64, &rd[0][0], 4));
    ASSERT_EQ(AJI_NO_ERROR, aji_flush(open));
    for (int i = 0; i < 8; ++i)
        EXPECT_EQ(0, rd[0][i]);
    for (int b = 1; b < 4; ++b)
        EXPECT_EQ(0, std::memcmp(wr[b - 1], rd[b], 8)) << "batch item " << b;
}

INSTANTIATE_TEST_SUITE_P(AllStyles, PackStyle,
                         ::testing::Values(AJI_PACK_NEVER, AJI_PACK_AUTO, AJI_PACK_MANUAL, AJI_PACK_STREAM),
                         [](const auto & info) {
                             switch (info.param) {
                             case AJI_PACK_NEVER: return "Never";
                             case AJI_PACK_AUTO: return "Auto";
                             case AJI_PACK_MANUAL: return "Manual";
                             default: return "Stream";
                             }
                         });

// ------------------------------------------------ every protocol version the mock supports

class ProtocolVersion : public RoundTrip, public ::testing::WithParamInterface<std::uint32_t> {};

TEST_P(ProtocolVersion, FullDeviceFlow)
{
    Scenario s = Scenario::default_board();
    s.version = GetParam();
    start(s);

    AJI_OPEN_ID open = open_locked(fpga_tap, claims_for({fpga_idcode_ir, fpga_user_ir}), AJI_PACK_MANUAL);
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, fpga_idcode_ir, nullptr));
    BYTE idcode[4] = {};
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 32, 0, 0, 0, nullptr, 0, 32, idcode));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, fpga_user_ir, nullptr));
    BYTE wr[2][2] = {{0xAA, 0x55}, {0x12, 0x34}}, rd[2][2] = {};
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 16, 0, 0, 16, &wr[0][0], 0, 16, &rd[0][0], 2));
    ASSERT_EQ(AJI_NO_ERROR, aji_run_test_idle(open, 10));
    ASSERT_EQ(AJI_NO_ERROR, aji_flush(open));
    EXPECT_EQ(0x02D120DDu, le32(idcode));
    EXPECT_EQ(0xAA, rd[1][0]);
    EXPECT_EQ(0x55, rd[1][1]);
    EXPECT_EQ(AJI_NO_ERROR, aji_unlock(open));
    EXPECT_EQ(AJI_NO_ERROR, aji_close_device(open));
}

// 2: claims as (type,value); 5: DR batch field; 10: weak claims; 11: DR timestamps;
// 12: RUN_TEST_IDLE flags; 13: 64-bit claims.
INSTANTIATE_TEST_SUITE_P(Versions, ProtocolVersion, ::testing::Values(2u, 4u, 5u, 10u, 11u, 12u, 13u),
                         [](const auto & info) { return std::format("v{}", info.param); });

// ------------------------------------------------ individual behaviours

TEST_F(RoundTrip, BypassDelaysByOneBit)
{
    start();
    AJI_OPEN_ID open = open_locked(fpga_tap, claims_for({fpga_bypass_ir}));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, fpga_bypass_ir, nullptr));
    const BYTE wr[2] = {0xA5, 0x01};
    BYTE rd[2] = {};
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 9, 0, 0, 9, wr, 0, 9, rd));
    EXPECT_EQ(0x4A, rd[0]); // captured 0, then 0xA5 shifted up by one
    EXPECT_EQ(0x01, rd[1] & 0x01);
}

TEST_F(RoundTrip, PartialReadAndWriteWindows)
{
    start();
    AJI_OPEN_ID open = open_locked(fpga_tap, claims_for({fpga_user_ir}));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, fpga_user_ir, nullptr));
    const BYTE wr[1] = {0xFF};
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 32, AJI_DR_UNUSED_0, 8, 8, wr, 0, 0, nullptr));
    BYTE rd[1] = {};
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 32, AJI_DR_UNUSED_0, 0, 0, nullptr, 4, 8, rd));
    EXPECT_EQ(0xF0, rd[0]); // bits 4..11 of a register with bits 8..15 set
}

TEST_F(RoundTrip, LargeTransfersSpanManyFifoPackets)
{
    start();
    AJI_OPEN_ID open = open_locked(fpga_tap, claims_for({fpga_user_ir}), AJI_PACK_MANUAL);
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, fpga_user_ir, nullptr));

    constexpr DWORD bits = 20000 * 8; // 20 kB each way, ~5 FIFO packets
    std::vector<BYTE> wr(bits / 8), rd(bits / 8);
    std::iota(wr.begin(), wr.end(), BYTE{7});
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, bits, 0, 0, bits, wr.data(), 0, 0, nullptr));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, bits, 0, 0, 0, nullptr, 0, bits, rd.data()));
    ASSERT_EQ(AJI_NO_ERROR, aji_flush(open));
    EXPECT_EQ(wr, rd);
}

TEST_F(RoundTrip, ManualPackingSendsOnePacket)
{
    start();
    AJI_OPEN_ID open = open_locked(fpga_tap, claims_for({fpga_user_ir}), AJI_PACK_MANUAL);
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, fpga_user_ir, nullptr));
    const BYTE wr[4] = {1, 2, 3, 4};
    for (int i = 0; i < 10; ++i)
        ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 32, 0, 0, 32, wr, 0, 0, nullptr));
    ASSERT_EQ(AJI_NO_ERROR, aji_flush(open));

    const auto drs = commands_of(Cmd::access_dr);
    ASSERT_EQ(10u, drs.size());
    for (const auto & r : drs)
        EXPECT_EQ(drs[0].packet, r.packet);
    EXPECT_EQ(drs[0].packet, commands_of(Cmd::access_ir).back().packet) << "IR rides in the same batch";
}

TEST_F(RoundTrip, StreamModePipelinesWithContinueCommands)
{
    start();
    AJI_OPEN_ID open = open_locked(fpga_tap, claims_for({fpga_user_ir}), AJI_PACK_STREAM);
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, fpga_user_ir, nullptr));
    std::vector<BYTE> wr(2048, 0x5A);
    for (int i = 0; i < 4; ++i)
        ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 2048 * 8, 0, 0, 2048 * 8, wr.data(), 0, 0, nullptr));
    ASSERT_EQ(AJI_NO_ERROR, aji_flush(open));
    EXPECT_FALSE(commands_of(Cmd::continue_commands).empty());
    EXPECT_EQ(4u, commands_of(Cmd::access_dr).size());
}

TEST_F(RoundTrip, TestLogicResetSelectsIdcode)
{
    start();
    AJI_OPEN_ID open = open_locked(fpga_tap, claims_for({fpga_user_ir, fpga_idcode_ir}));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, fpga_user_ir, nullptr));
    ASSERT_EQ(AJI_NO_ERROR, aji_test_logic_reset(open));
    ASSERT_EQ(AJI_NO_ERROR, aji_run_test_idle(open, 5));
    ASSERT_EQ(AJI_NO_ERROR, aji_delay(open, 1000));
    BYTE idcode[4] = {};
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 32, 0, 0, 0, nullptr, 0, 32, idcode));
    EXPECT_EQ(0x02D120DDu, le32(idcode));
}

TEST_F(RoundTrip, WholeChainScans)
{
    start();
    AJI_CHAIN_ID chain = locked_chain();
    AJI_OPEN_ID open = nullptr;
    ASSERT_EQ(AJI_NO_ERROR, aji_open_entire_device_chain(chain, &open, AJI_CHAIN_JTAG, "roundtrip"));
    ASSERT_EQ(AJI_NO_ERROR, aji_unlock_chain_lock(chain, open, AJI_PACK_NEVER));

    // IR: 4 bits HPS (nearest TDO) + 10 bits FPGA, all ones = BYPASS on both.
    const BYTE ir_all_ones[2] = {0xFF, 0x3F};
    BYTE ir_capture[2] = {};
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, 14, ir_all_ones, ir_capture));
    EXPECT_EQ(0x11, ir_capture[0]); // 'b0001 from each TAP
    EXPECT_EQ(0x00, ir_capture[1]);

    // DR: two bypass bits, so the pattern comes out two bits late.
    const BYTE wr[1] = {0xFF};
    BYTE rd[2] = {};
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 10, 0, 0, 8, wr, 0, 10, rd));
    EXPECT_EQ(0xFC, rd[0]);
    EXPECT_EQ(0x03, rd[1]);
}

TEST_F(RoundTrip, ExclusiveClaimsConflictBetweenOpens)
{
    start();
    AJI_CHAIN_ID chain = locked_chain();
    AJI_CLAIM2 exclusive{AJI_CLAIM_IR, 0, fpga_user_ir};
    AJI_OPEN_ID first = nullptr, second = nullptr;
    ASSERT_EQ(AJI_NO_ERROR, aji_open_device(chain, fpga_tap, &first, &exclusive, 1, "first-app"));
    EXPECT_EQ(AJI_CHAINS_CLAIMED, aji_open_device(chain, fpga_tap, &second, &exclusive, 1, "second-app"));
    EXPECT_STREQ("first-app on 127.0.0.1", aji_get_error_info());

    AJI_CLAIM2 shared{AJI_CLAIM_IR_SHARED, 0, fpga_idcode_ir};
    AJI_OPEN_ID third = nullptr, fourth = nullptr;
    EXPECT_EQ(AJI_NO_ERROR, aji_open_device(chain, fpga_tap, &third, &shared, 1, "third-app"));
    EXPECT_EQ(AJI_NO_ERROR, aji_open_device(chain, fpga_tap, &fourth, &shared, 1, "fourth-app"));
}

TEST_F(RoundTrip, LockHandoverBetweenDevices)
{
    start();
    AJI_CHAIN_ID chain = locked_chain();
    AJI_CLAIM2 claim{AJI_CLAIM_IR_SHARED, 0, 0};
    AJI_OPEN_ID hps = nullptr, fpga = nullptr;
    claim.value = 0xE;
    ASSERT_EQ(AJI_NO_ERROR, aji_open_device(chain, 0, &hps, &claim, 1, "roundtrip"));
    claim.value = fpga_idcode_ir;
    ASSERT_EQ(AJI_NO_ERROR, aji_open_device(chain, fpga_tap, &fpga, &claim, 1, "roundtrip"));

    ASSERT_EQ(AJI_NO_ERROR, aji_unlock_chain_lock(chain, hps, AJI_PACK_NEVER));
    BYTE id[4] = {};
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(hps, 0xE, nullptr));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(hps, 32, 0, 0, 0, nullptr, 0, 32, id));
    EXPECT_EQ(0x4BA00477u, le32(id));

    ASSERT_EQ(AJI_NO_ERROR, aji_unlock_lock(hps, fpga));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(fpga, fpga_idcode_ir, nullptr));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(fpga, 32, 0, 0, 0, nullptr, 0, 32, id));
    EXPECT_EQ(0x02D120DDu, le32(id));

    ASSERT_EQ(AJI_NO_ERROR, aji_unlock_lock_chain(fpga, chain));
    EXPECT_EQ(AJI_NO_ERROR, aji_unlock_chain(chain));
}

} // namespace
} // namespace roundtrip
