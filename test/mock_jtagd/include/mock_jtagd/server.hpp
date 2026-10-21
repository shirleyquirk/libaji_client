// In-process mock of the Quartus JTAG server (jtagd / jtagserver.exe).
//
// Speaks the AJI client/server protocol on a TCP port and serves the cables
// and chains described by a Scenario. Each accepted connection runs on its own
// thread; lock/claim state is shared between connections like the real server.
#pragma once

#include "mock_jtagd/protocol.hpp"
#include "mock_jtagd/scenario.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mock_jtagd {

// Make the server misbehave on purpose.
struct Fault {
    Cmd cmd;
    Err error = Err::failure;   // response code to return instead of executing
    unsigned skip = 0;          // let this many matching commands through first
    bool disconnect = false;    // drop the connection instead of responding
};

// One command as the server saw it, for test assertions.
struct CommandRecord {
    unsigned session;   // 1-based connection number
    unsigned packet;    // 1-based mux-0 packet number within the session
    Cmd cmd;
    Err result;
    bool executed;      // false if skipped because an earlier command in the batch failed
};

struct ServerOptions {
    std::uint16_t port = 0;                 // 0 = pick an ephemeral port
    std::string bind_address = "127.0.0.1";
    bool trace = false;                     // log every command to stderr
};

class Server {
public:
    explicit Server(Scenario scenario, ServerOptions options = {});
    ~Server();
    Server(const Server &) = delete;
    Server & operator=(const Server &) = delete;

    std::uint16_t port() const;
    void stop();

    void inject(Fault fault);
    std::vector<CommandRecord> commands() const;
    unsigned connections() const;

    // Chain id the client sees for scenario cable `index` (they are 1-based).
    static std::uint32_t chain_id(std::size_t index) { return static_cast<std::uint32_t>(index + 1); }

    struct Impl; // implementation detail, public only so server.cpp's session code can name it

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace mock_jtagd
