// Round-trip tests: the real libaji_client talking to the in-process mock jtagd.
//
// libaji_client holds process-global connection state, so each test must run
// in its own process (CMake registers one ctest per test case).
#pragma once

#include "aji.h"
#include "mock_jtagd/scenario_yaml.hpp"
#include "mock_jtagd/server.hpp"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace roundtrip {

using namespace mock_jtagd;

class RoundTrip : public ::testing::Test {
protected:
    // Start the mock and point the client at it. Call before any aji_* function.
    // With `client_password`, the server is configured as Remote1 (the only way
    // the client sends a password) instead of replacing the local server.
    // With `register_server` false the client config is left empty and the local
    // server disabled, for tests that add the server through the API instead.
    void start(Scenario scenario = Scenario::default_board(),
               std::optional<std::string> client_password = std::nullopt, bool register_server = true)
    {
        server_ = std::make_unique<Server>(std::move(scenario), ServerOptions{.trace = trace_enabled()});
        config_path_ = std::filesystem::path(::testing::TempDir()) /
                       std::format("aji_client_{}.conf", ::getpid());
        std::ofstream conf(config_path_);
        if (!register_server) {
            ::setenv("QUARTUS_JTAG_CLIENT_NO_LOCAL_SERVER", "1", 1);
        } else if (client_password) {
            conf << std::format("Remote1 {{\n  Host = \"127.0.0.1:{}\";\n  Password = \"{}\";\n}}\n",
                                server_->port(), *client_password);
            ::setenv("QUARTUS_JTAG_CLIENT_NO_LOCAL_SERVER", "1", 1);
        } else {
            // Replacing (rather than adding to) the local server also stops the
            // client from trying to launch a real jtagd when it can't connect.
            conf << std::format("ReplaceLocalJtagServer = \"127.0.0.1:{}\";\n", server_->port());
        }
        conf.close();
        ::setenv("QUARTUS_JTAG_CLIENT_CONFIG", config_path_.c_str(), 1);
    }

    // Same, with the hardware described by a YAML file in test/scenarios/.
    void start(const std::string & scenario_name)
    {
        start(load_scenario(std::filesystem::path(MOCK_JTAGD_SCENARIO_DIR) / scenario_name));
    }

    void TearDown() override
    {
        if (server_) {
            // Anything left here means the code talked to hardware the scenario
            // doesn't describe (e.g. a request no transcript entry matches).
            for (const auto & e : server_->take_model_errors())
                ADD_FAILURE() << "mock model error: " << e;
            server_->stop();
        }
        if (!config_path_.empty())
            std::filesystem::remove(config_path_);
    }

    std::vector<AJI_HARDWARE> hardware()
    {
        DWORD n = 8;
        std::vector<AJI_HARDWARE> hw(n);
        EXPECT_EQ(AJI_NO_ERROR, aji_get_hardware(&n, hw.data(), 5000));
        hw.resize(n);
        return hw;
    }

    // First (real) chain, locked and scanned.
    AJI_CHAIN_ID locked_chain()
    {
        auto hw = hardware();
        EXPECT_FALSE(hw.empty());
        if (hw.empty())
            return nullptr;
        AJI_CHAIN_ID chain = hw[0].chain_id;
        EXPECT_EQ(AJI_NO_ERROR, aji_lock_chain(chain, 1000));
        DWORD count = 8;
        AJI_DEVICE devices[8];
        EXPECT_EQ(AJI_NO_ERROR, aji_read_device_chain(chain, &count, devices, true));
        return chain;
    }

    // Open a TAP of the first chain and take the device lock.
    AJI_OPEN_ID open_locked(DWORD tap, std::vector<AJI_CLAIM2> claims,
                            AJI_PACK_STYLE pack = AJI_PACK_NEVER)
    {
        AJI_CHAIN_ID chain = locked_chain();
        AJI_OPEN_ID open = nullptr;
        EXPECT_EQ(AJI_NO_ERROR,
                  aji_open_device(chain, tap, &open, claims.data(), static_cast<DWORD>(claims.size()), "roundtrip"));
        // Not aji_unlock_chain_lock(): that path stores AJI_PACK_STREAM verbatim
        // instead of translating it the way aji_lock() does, so it never streams.
        EXPECT_EQ(AJI_NO_ERROR, aji_unlock_chain(chain));
        EXPECT_EQ(AJI_NO_ERROR, aji_lock(open, 1000, pack));
        return open;
    }

    std::vector<CommandRecord> commands_of(Cmd cmd) const
    {
        std::vector<CommandRecord> out;
        for (const auto & r : server_->commands())
            if (r.cmd == cmd)
                out.push_back(r);
        return out;
    }

    static bool trace_enabled() { return std::getenv("MOCK_JTAGD_TRACE") != nullptr; }

    std::unique_ptr<Server> server_;
    std::filesystem::path config_path_;
};

// A second, hand-rolled connection to the mock, for things one libaji process
// can't do to itself (e.g. hold a chain lock the client then collides with).
class RawClient {
public:
    explicit RawClient(std::uint16_t port)
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
            throw std::runtime_error("RawClient: connect failed");
        receive(); // greeting
    }
    ~RawClient() { ::close(fd_); }

    // Send one command block, return the first response block's error code.
    std::uint8_t command(Cmd cmd, std::initializer_list<std::uint32_t> args)
    {
        Writer w;
        w.begin_block(static_cast<std::uint8_t>(cmd));
        for (auto a : args)
            w.u32(a);
        w.end_block();
        const auto bytes = w.bytes();
        const unsigned hdr = static_cast<unsigned>(bytes.size() - 1);
        std::vector<std::uint8_t> pkt{static_cast<std::uint8_t>(hdr >> 8), static_cast<std::uint8_t>(hdr)};
        pkt.insert(pkt.end(), bytes.begin(), bytes.end());
        if (::send(fd_, pkt.data(), pkt.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(pkt.size()))
            throw std::runtime_error("RawClient: send failed");
        return receive().at(0);
    }

private:
    std::vector<std::uint8_t> receive()
    {
        for (;;) {
            std::uint8_t hdr[2];
            read_exact(hdr, 2);
            std::vector<std::uint8_t> payload(((hdr[0] & 0x0F) << 8 | hdr[1]) + 1);
            read_exact(payload.data(), payload.size());
            if ((hdr[0] >> 4) == 0)
                return payload;
        }
    }
    void read_exact(std::uint8_t * p, std::size_t n)
    {
        while (n > 0) {
            const ssize_t r = ::recv(fd_, p, n, 0);
            if (r <= 0)
                throw std::runtime_error("RawClient: connection closed");
            p += r;
            n -= static_cast<std::size_t>(r);
        }
    }
    int fd_ = -1;
};

inline std::uint32_t le32(const BYTE * p)
{
    return std::uint32_t{p[0]} | std::uint32_t{p[1]} << 8 | std::uint32_t{p[2]} << 16 | std::uint32_t{p[3]} << 24;
}

} // namespace roundtrip
