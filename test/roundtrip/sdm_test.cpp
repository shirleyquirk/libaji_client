// Hello world for testing your own code: the toy SDM client (standing in for a
// real wrapper) against a board described in scenarios/sdm_hello.yaml.
//
// What this proves: node discovery, node open/claims, virtual-IR selection and
// word framing through USER1/USER0 work against the real client stack. What it
// does not prove: anything about real SDM behaviour; the transcript is a
// placeholder until replaced by captured traffic.
#include "fixture.hpp"
#include "toy_sdm_client.hpp"

namespace roundtrip {
namespace {

TEST_F(RoundTrip, SdmHelloWorldReadsIdcode)
{
    start("sdm_hello.yaml");
    toy::SdmClient sdm;
    ASSERT_EQ(AJI_NO_ERROR, sdm.find());
    EXPECT_EQ(sld_node_idcode(1, toy::sdm_node_id, mfg_altera, 0), sdm.node_idcode());
    ASSERT_EQ(AJI_NO_ERROR, sdm.open());

    toy::Response r;
    ASSERT_EQ(AJI_NO_ERROR, sdm.call(toy::get_idcode, {}, r));
    EXPECT_EQ(0u, r.error);
    ASSERT_EQ(1u, r.data.size());
    EXPECT_EQ(0x4341A0DDu, r.data[0]);
}

TEST_F(RoundTrip, SdmResponseIsIdleWhenNothingIsPending)
{
    start("sdm_hello.yaml");
    toy::SdmClient sdm;
    ASSERT_EQ(AJI_NO_ERROR, sdm.find());
    ASSERT_EQ(AJI_NO_ERROR, sdm.open());
    DWORD word = ~0u;
    ASSERT_EQ(AJI_NO_ERROR, sdm.read_idle(word));
    EXPECT_EQ(0u, word);
}

TEST_F(RoundTrip, SdmRequestOutsideTranscriptIsAModelError)
{
    // The mock doesn't guess: a command it has no recording for is reported,
    // and the test fails unless it expects that.
    start("sdm_hello.yaml");
    toy::SdmClient sdm;
    ASSERT_EQ(AJI_NO_ERROR, sdm.find());
    ASSERT_EQ(AJI_NO_ERROR, sdm.open());
    toy::Response r;
    ASSERT_EQ(AJI_NO_ERROR, sdm.call(0x99, {1, 2}, r));
    EXPECT_TRUE(r.data.empty()); // idle header: no data, error 0

    const auto errors = server_->take_model_errors();
    ASSERT_EQ(1u, errors.size());
    EXPECT_NE(std::string::npos, errors[0].find("node 'sdm' channel 'packets'")) << errors[0];
    EXPECT_NE(std::string::npos, errors[0].find("0x00002099")) << errors[0];
}

TEST_F(RoundTrip, SdmFindFailsWithoutTheNode)
{
    start("default_board.yaml");
    toy::SdmClient sdm;
    EXPECT_EQ(AJI_NO_DEVICES, sdm.find());
}

TEST_F(RoundTrip, ConstRegisterOnNode)
{
    start("sdm_hello.yaml");
    toy::SdmClient sdm;
    ASSERT_EQ(AJI_NO_ERROR, sdm.find());
    AJI_CHAIN_ID chain = sdm.chain(); // still locked by find()
    const AJI_CLAIM claims[] = {{AJI_CLAIM_OVERLAY, 0}};
    AJI_OPEN_ID node = nullptr;
    ASSERT_EQ(AJI_NO_ERROR, aji_open_node(chain, 0, sdm.node_idcode(), &node, claims, 1, "const-reg"));
    ASSERT_EQ(AJI_NO_ERROR, aji_unlock_chain_lock(chain, node, AJI_PACK_NEVER));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_overlay(node, 0, nullptr));
    BYTE rd[4] = {};
    const BYTE wr[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(node, 32, 0, 0, 32, wr, 0, 32, rd));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(node, 32, 0, 0, 0, nullptr, 0, 32, rd));
    EXPECT_EQ(0x05D30001u, le32(rd)) << "writes don't stick to a const register";
}

} // namespace
} // namespace roundtrip

namespace roundtrip {
namespace {

TEST_F(RoundTrip, TapLevelChannel)
{
    // Command/response on private TAP instructions rather than a hub node.
    start("sdm_tap.yaml");
    constexpr DWORD command_ir = 0x2B0, response_ir = 0x2B1;
    AJI_OPEN_ID open = open_locked(0, {{AJI_CLAIM_IR_SHARED, 0, command_ir}, {AJI_CLAIM_IR_SHARED, 0, response_ir}});

    const BYTE request[4] = {0x10, 0x00, 0x00, 0x00};
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, command_ir, nullptr));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 32, 0, 0, 32, request, 0, 0, nullptr));
    BYTE response[8] = {};
    ASSERT_EQ(AJI_NO_ERROR, aji_access_ir(open, response_ir, nullptr));
    ASSERT_EQ(AJI_NO_ERROR, aji_access_dr(open, 64, 0, 0, 0, nullptr, 0, 64, response));
    EXPECT_EQ(0x00001000u, le32(response));
    EXPECT_EQ(0x4341A0DDu, le32(response + 4));
}

} // namespace
} // namespace roundtrip
