// Connection, hardware enumeration, chain scanning and chain-level commands.
#include "fixture.hpp"

#include <cstring>

namespace roundtrip {
namespace {

TEST_F(RoundTrip, ListsTheMockCable)
{
    start();
    auto hw = hardware();
    ASSERT_EQ(1u, hw.size());
    EXPECT_STREQ("USB-Blaster", hw[0].hw_name);
    EXPECT_STREQ("USB-1", hw[0].port);
    EXPECT_EQ(AJI_CHAIN_JTAG, hw[0].chain_type);
    EXPECT_EQ(std::format("127.0.0.1:{}", server_->port()), hw[0].server);
    EXPECT_NE(0u, hw[0].persistent_id);
    EXPECT_EQ(0u, hw[0].features & AJI_FEATURE_DUMMY);
}

TEST_F(RoundTrip, HandshakeNegotiatesVersionAndReportsServerInfo)
{
    start();
    DWORD n = 4;
    AJI_HARDWARE hw[4];
    char * info[4] = {};
    ASSERT_EQ(AJI_NO_ERROR, aji_get_hardware2(&n, hw, info, 5000));
    ASSERT_EQ(1u, n);
    EXPECT_STREQ("Mock jtagd (libaji_client test server)", info[0]);
    delete[] info[0];

    const auto use = commands_of(Cmd::use_protocol_version);
    ASSERT_EQ(1u, use.size());
    const auto info_cmds = commands_of(Cmd::get_version_info);
    ASSERT_EQ(1u, info_cmds.size());
    EXPECT_EQ(use[0].packet, info_cmds[0].packet) << "client sends both in one packet";
}

TEST_F(RoundTrip, EnumeratesMultipleCables)
{
    Scenario s = Scenario::default_board();
    Cable second = s.cables[0];
    second.port = "USB-2";
    second.device_name = "bench";
    s.cables.push_back(second);
    start(s);

    auto hw = hardware();
    ASSERT_EQ(2u, hw.size());
    EXPECT_NE(hw[0].persistent_id, hw[1].persistent_id);

    // With ReplaceLocalJtagServer the cables belong to server "127.0.0.1:<port>",
    // so a bare "USB-Blaster [USB-2]" does not match; the printed name does.
    char name[256];
    ASSERT_EQ(AJI_NO_ERROR, aji_print_hardware_name(hw[1].chain_id, name, sizeof(name)));
    EXPECT_EQ(std::format("USB-Blaster on 127.0.0.1:{} [USB-2]", server_->port()), name);
    AJI_HARDWARE found{};
    ASSERT_EQ(AJI_NO_ERROR, aji_find_hardware(name, &found, 1000));
    EXPECT_STREQ("USB-2", found.port);
    EXPECT_STREQ("bench", found.device_name);
    EXPECT_EQ(AJI_NO_DEVICES, aji_find_hardware("USB-Blaster [USB-2]", &found, 1000));
}

TEST_F(RoundTrip, ReadsDeviceChain)
{
    start();
    AJI_CHAIN_ID chain = hardware().at(0).chain_id;
    ASSERT_EQ(AJI_NO_ERROR, aji_lock_chain(chain, 1000));

    DWORD n = 8;
    AJI_DEVICE dev[8];
    ASSERT_EQ(AJI_NO_ERROR, aji_read_device_chain(chain, &n, dev, true));
    ASSERT_EQ(2u, n);
    EXPECT_EQ(0x4BA00477u, dev[0].device_id);
    EXPECT_EQ(4, dev[0].instruction_length);
    EXPECT_STREQ("SOCVHPS", dev[0].device_name);
    EXPECT_EQ(0x02D120DDu, dev[1].device_id);
    EXPECT_EQ(10, dev[1].instruction_length);
    EXPECT_EQ(static_cast<DWORD>(AJI_DEVFEAT_POSSIBLE_HUB), dev[1].features);

    // Second read: client sends its tag, mock answers "unchanged" with no FIFO data.
    n = 8;
    ASSERT_EQ(AJI_NO_ERROR, aji_read_device_chain(chain, &n, dev, true));
    EXPECT_EQ(2u, n);
    EXPECT_EQ(0x02D120DDu, dev[1].device_id);
    EXPECT_EQ(AJI_NO_ERROR, aji_unlock_chain(chain));
}

TEST_F(RoundTrip, ReadChainWithoutAutoScanReportsNotConfigured)
{
    start();
    AJI_CHAIN_ID chain = hardware().at(0).chain_id;
    ASSERT_EQ(AJI_NO_ERROR, aji_lock_chain(chain, 1000));
    DWORD n = 8;
    AJI_DEVICE dev[8];
    EXPECT_EQ(AJI_CHAIN_NOT_CONFIGURED, aji_read_device_chain(chain, &n, dev, false));
}

TEST_F(RoundTrip, TooSmallDeviceBufferReportsCount)
{
    start();
    AJI_CHAIN_ID chain = hardware().at(0).chain_id;
    ASSERT_EQ(AJI_NO_ERROR, aji_lock_chain(chain, 1000));
    DWORD n = 1;
    AJI_DEVICE dev[1];
    EXPECT_EQ(AJI_TOO_MANY_DEVICES, aji_read_device_chain(chain, &n, dev, true));
    EXPECT_EQ(2u, n);
}

TEST_F(RoundTrip, ChainParameters)
{
    start();
    AJI_CHAIN_ID chain = locked_chain();
    DWORD value = 0;
    ASSERT_EQ(AJI_NO_ERROR, aji_get_parameter(chain, "JtagClock", &value));
    EXPECT_EQ(6000000u, value);
    ASSERT_EQ(AJI_NO_ERROR, aji_set_parameter(chain, "JtagClock", 24000000));
    ASSERT_EQ(AJI_NO_ERROR, aji_get_parameter(chain, "JtagClock", &value));
    EXPECT_EQ(24000000u, value);
    EXPECT_NE(AJI_NO_ERROR, aji_get_parameter(chain, "NoSuchParameter", &value));
}

TEST_F(RoundTrip, ChainCommandsNeedTheChainLock)
{
    start();
    AJI_CHAIN_ID chain = hardware().at(0).chain_id;
    DWORD value = 0;
    EXPECT_EQ(AJI_NOT_LOCKED, aji_get_parameter(chain, "JtagClock", &value)); // client-side check
    EXPECT_EQ(AJI_NOT_LOCKED, aji_unlock_chain(chain));
}

TEST_F(RoundTrip, DefinedDevicesRoundTrip)
{
    start();
    hardware(); // connect
    AJI_DEVICE dev{};
    dev.device_id = 0x12345679;
    dev.instruction_length = 8;
    dev.device_name = "MOCKDEV";
    ASSERT_EQ(AJI_NO_ERROR, aji_define_device(&dev));

    DWORD n = 8;
    AJI_DEVICE list[8];
    ASSERT_EQ(AJI_NO_ERROR, aji_get_defined_devices(&n, list));
    ASSERT_EQ(1u, n);
    EXPECT_EQ(0x12345679u, list[0].device_id);
    EXPECT_EQ(8, list[0].instruction_length);
    EXPECT_STREQ("MOCKDEV", list[0].device_name);

    ASSERT_EQ(AJI_NO_ERROR, aji_undefine_device(&dev));
    n = 8;
    ASSERT_EQ(AJI_NO_ERROR, aji_get_defined_devices(&n, list));
    EXPECT_EQ(0u, n);
}

TEST_F(RoundTrip, ChainLockHeldByAnotherClientReportsBlocker)
{
    start();
    AJI_CHAIN_ID chain = hardware().at(0).chain_id;

    RawClient other(server_->port());
    ASSERT_EQ(static_cast<std::uint8_t>(Err::no_error), other.command(Cmd::lock_chain, {Server::chain_id(0), 0}));

    EXPECT_EQ(AJI_CHAIN_IN_USE, aji_lock_chain(chain, 200));
    // Blocker info: app name and address of whoever holds the lock.
    EXPECT_STREQ("mock-session-2 on 127.0.0.1", aji_get_error_info());

    ASSERT_EQ(static_cast<std::uint8_t>(Err::no_error), other.command(Cmd::unlock_chain, {Server::chain_id(0)}));
    EXPECT_EQ(AJI_NO_ERROR, aji_lock_chain(chain, 200));
}

TEST_F(RoundTrip, LockWaitsForOtherClientToRelease)
{
    start();
    AJI_CHAIN_ID chain = hardware().at(0).chain_id;

    auto other = std::make_unique<RawClient>(server_->port());
    ASSERT_EQ(0, other->command(Cmd::lock_chain, {Server::chain_id(0), 0}));
    std::jthread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        other.reset(); // disconnecting drops the lock
    });
    EXPECT_EQ(AJI_NO_ERROR, aji_lock_chain(chain, 5000));
}

TEST_F(RoundTrip, Md5AuthenticationSucceedsWithRightPassword)
{
    Scenario s = Scenario::default_board();
    s.password = "hunter2";
    start(s, "hunter2");
    auto hw = hardware();
    ASSERT_EQ(1u, hw.size());
    EXPECT_STREQ("USB-Blaster", hw[0].hw_name);
    const auto auth = commands_of(Cmd::authenticate_md5);
    ASSERT_EQ(1u, auth.size());
    EXPECT_EQ(Err::no_error, auth[0].result);
}

TEST_F(RoundTrip, Md5AuthenticationFailsWithWrongPassword)
{
    Scenario s = Scenario::default_board();
    s.password = "hunter2";
    start(s, "wrong");
    auto hw = hardware();
    ASSERT_EQ(1u, hw.size());
    EXPECT_NE(0u, hw[0].features & AJI_FEATURE_DUMMY);
    EXPECT_STREQ("Authentication failure", hw[0].port);
}

TEST_F(RoundTrip, TemporaryRemoteServerViaApi)
{
    Scenario s = Scenario::default_board();
    s.password = "hunter2";
    start(s, std::nullopt, false);
    const std::string host = std::format("127.0.0.1:{}", server_->port());
    {
        // The client must copy the password: this buffer is gone before it connects.
        std::string password = "hunter2";
        ASSERT_EQ(AJI_NO_ERROR, aji_add_remote_server(host.c_str(), password.c_str(), true));
        password.assign(password.size(), 'X');
    }
    // Re-adding replaces the existing temporary entry.
    ASSERT_EQ(AJI_NO_ERROR, aji_add_remote_server(host.c_str(), "hunter2", true));

    auto hw = hardware();
    ASSERT_EQ(1u, hw.size());
    EXPECT_STREQ("USB-Blaster", hw[0].hw_name);
    EXPECT_STREQ(host.c_str(), hw[0].server);
    ASSERT_EQ(1u, commands_of(Cmd::authenticate_md5).size());
    EXPECT_EQ(Err::no_error, commands_of(Cmd::authenticate_md5)[0].result);
}

} // namespace
} // namespace roundtrip
