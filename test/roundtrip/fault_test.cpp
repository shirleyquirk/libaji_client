// Injected server errors and dropped connections: what the client reports.
#include "fixture.hpp"

namespace roundtrip {
namespace {

constexpr DWORD fpga_tap = 1;
constexpr DWORD fpga_user_ir = 0x055;

AJI_CLAIM2 shared(DWORD ir) { return {AJI_CLAIM_IR_SHARED, 0, ir}; }

TEST_F(RoundTrip, InjectedErrorIsReturnedUnbatched)
{
    start();
    AJI_OPEN_ID open = open_locked(fpga_tap, {shared(fpga_user_ir)});
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, fpga_user_ir, nullptr));
    server_->inject({.cmd = Cmd::access_dr, .error = Err::bad_jtag_chain});
    BYTE rd[4];
    EXPECT_EQ(AJI_BAD_JTAG_CHAIN, aji_access_dr(open, 32, 0, 0, 0, nullptr, 0, 32, rd));
}

TEST_F(RoundTrip, ErrorInBatchSkipsTheRestAndSurfacesOnFlush)
{
    start();
    AJI_OPEN_ID open = open_locked(fpga_tap, {shared(fpga_user_ir)}, AJI_PACK_MANUAL);
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, fpga_user_ir, nullptr));
    server_->inject({.cmd = Cmd::access_dr, .error = Err::failure, .skip = 1});
    const BYTE wr[4] = {1, 2, 3, 4};
    for (int i = 0; i < 3; ++i)
        ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 32, 0, 0, 32, wr, 0, 0, nullptr)) << "deferred";
    EXPECT_EQ(AJI_FAILURE, aji_flush(open));

    const auto drs = commands_of(Cmd::access_dr);
    ASSERT_EQ(3u, drs.size());
    EXPECT_TRUE(drs[0].executed);
    EXPECT_EQ(Err::failure, drs[1].result);
    EXPECT_FALSE(drs[2].executed);
}

TEST_F(RoundTrip, SkippedCommandsStillConsumeTheirFifoData)
{
    // If the server didn't drain the skipped command's TX FIFO bytes, the next
    // batch would read stale data and this second round would come back wrong.
    start();
    AJI_OPEN_ID open = open_locked(fpga_tap, {shared(fpga_user_ir)}, AJI_PACK_MANUAL);
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, fpga_user_ir, nullptr));
    ASSERT_EQ(AJI_NO_ERROR, aji_flush(open));
    server_->inject({.cmd = Cmd::access_dr, .error = Err::failure});
    const BYTE junk[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 32, 0, 0, 32, junk, 0, 0, nullptr));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 32, 0, 0, 32, junk, 0, 0, nullptr));
    EXPECT_EQ(AJI_FAILURE, aji_flush(open));

    // The device lock survives a failed batch; carry on with the same open.
    const BYTE wr[4] = {0x11, 0x22, 0x33, 0x44};
    BYTE rd[4] = {};
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, fpga_user_ir, nullptr));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 32, 0, 0, 32, wr, 0, 0, nullptr));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 32, 0, 0, 0, nullptr, 0, 32, rd));
    ASSERT_EQ(AJI_NO_ERROR, aji_flush(open));
    EXPECT_EQ(0x44332211u, le32(rd));
}

TEST_F(RoundTrip, DroppedConnectionIsReported)
{
    start();
    AJI_OPEN_ID open = open_locked(fpga_tap, {shared(fpga_user_ir)});
    server_->inject({.cmd = Cmd::access_ir, .disconnect = true});
    EXPECT_NE(AJI_NO_ERROR, aji_access_ir(open, fpga_user_ir, nullptr));
}

TEST_F(RoundTrip, ClientReconnectsAfterServerDropsLink)
{
    start();
    ASSERT_EQ(1u, hardware().size());
    server_->inject({.cmd = Cmd::lock_chain, .disconnect = true});
    AJI_CHAIN_ID chain = hardware().at(0).chain_id;
    EXPECT_NE(AJI_NO_ERROR, aji_lock_chain(chain, 1000));
    // The client backs off for a few seconds before reconnecting.
    for (int i = 0; i < 100 && server_->connections() < 2; ++i) {
        hardware();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_EQ(2u, server_->connections());
}

} // namespace
} // namespace roundtrip
