// Wrapper tests that need no JTAG hardware and no jtagd.
//
// They run against the real libaji_client. Servers are registered at
// localhost:1 / localhost:2, where nothing listens, so the library reports a
// dummy chain ("Unable to connect") per server: a real library-owned chain
// object, which is what the lifetime guards are about. Locking, opening and
// scanning need real hardware: see ../examples.
//
// One process per case (the library's server list is global): ctest runs
//   aji_no_hardware <case>
// with QUARTUS_JTAG_CLIENT_NO_LOCAL_SERVER=1 and a private
// QUARTUS_JTAG_CLIENT_CONFIG file.
#include "aji/aji.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

int failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

#define CHECK_ERRC(result, errc)                                                       \
    do {                                                                               \
        auto&& r_ = (result);                                                          \
        if (r_) {                                                                      \
            std::fprintf(stderr, "%s:%d: expected %s, got success\n", __FILE__, __LINE__, \
                         std::string(aji::to_string(errc)).c_str());                   \
            ++failures;                                                                \
        } else if (r_.error().code != (errc)) {                                        \
            std::fprintf(stderr, "%s:%d: expected %s, got %s\n", __FILE__, __LINE__,   \
                         std::string(aji::to_string(errc)).c_str(),                    \
                         r_.error().message().c_str());                                \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

const char* config_path()
{
    const char* p = std::getenv("QUARTUS_JTAG_CLIENT_CONFIG");
    if (p == nullptr) {
        std::fputs("QUARTUS_JTAG_CLIENT_CONFIG not set; run via ctest\n", stderr);
        std::exit(2);
    }
    return p;
}

void write_config(const std::string& text)
{
    FILE* f = std::fopen(config_path(), "w");
    if (f == nullptr) {
        std::perror(config_path());
        std::exit(2);
    }
    std::fputs(text.c_str(), f);
    std::fclose(f);
}

std::string remote(int n, const char* host)
{
    return "Remote" + std::to_string(n) + " {\n\tHost = \"" + host + "\";\n\tPassword = \"pw\";\n}\n";
}

bool on(const aji::HardwareInfo& h, const char* server) { return h.server && *h.server == server; }

// ---------------------------------------------------------------------------

void scan_copies_info()
{
    write_config("");
    CHECK(aji::add_remote_server("localhost:1", "pw", aji::Persist::temporary));
    auto chains = aji::scan(2s);
    CHECK(chains);
    if (!chains)
        return;
    CHECK(chains->size() == 1);
    const aji::HardwareInfo& info = chains->front().info();
    CHECK(info.is_dummy());
    CHECK(on(info, "localhost:1"));
    CHECK(info.hw_name.empty());
}

void management_refused_while_chains_alive()
{
    write_config("");
    CHECK(aji::add_remote_server("localhost:1", "pw", aji::Persist::temporary));
    {
        auto chains = aji::scan(2s);
        CHECK(chains && !chains->empty());

        CHECK_ERRC(aji::scan(2s), aji::Errc::chains_in_use);
        CHECK_ERRC(aji::find([](auto&) { return true; }), aji::Errc::chains_in_use);
        CHECK_ERRC(aji::find_by_name("x", 1ms), aji::Errc::chains_in_use);
        CHECK_ERRC(aji::find_by_persistent_id(1, 1ms), aji::Errc::chains_in_use);
        CHECK_ERRC(aji::add_remote_server("localhost:2", "pw", aji::Persist::temporary), aji::Errc::chains_in_use);
        CHECK_ERRC(aji::define_device({.idcode = 0x1234567F, .ir_length = 10, .name = "x"}), aji::Errc::chains_in_use);
        CHECK_ERRC(aji::defined_devices(), aji::Errc::chains_in_use);
        CHECK_ERRC(aji::remote_clients_enabled(), aji::Errc::chains_in_use);
        CHECK_ERRC(aji::add_hardware({.hw_name = "x"}), aji::Errc::chains_in_use);
    }
    CHECK(aji::scan(2s));   // all handles gone
}

void copies_and_moves_pin()
{
    write_config("");
    CHECK(aji::add_remote_server("localhost:1", "pw", aji::Persist::temporary));
    auto chains = aji::scan(2s);
    CHECK(chains && chains->size() == 1);
    if (!chains || chains->empty())
        return;

    aji::Chain moved = std::move(chains->front());
    chains->clear();
    CHECK(moved);
    CHECK(aji::detail::Pin::count() == 1);

    {
        aji::Chain copy = moved;
        CHECK(aji::detail::Pin::count() == 2);
        CHECK_ERRC(aji::remove(copy), aji::Errc::chains_in_use);   // `moved` still holds one
    }
    CHECK(aji::detail::Pin::count() == 1);

    aji::Chain other = std::move(moved);
    CHECK(!moved);
    CHECK(aji::detail::Pin::count() == 1);
}

void find_matches_exactly_one()
{
    write_config("");
    CHECK(aji::add_remote_server("localhost:1", "pw", aji::Persist::temporary));
    CHECK(aji::add_remote_server("localhost:2", "pw", aji::Persist::temporary));
    {
        auto two = aji::find([](const aji::HardwareInfo& h) { return on(h, "localhost:2"); });
        CHECK(two);
        if (two)
            CHECK(on(two->info(), "localhost:2"));
    }
    CHECK_ERRC(aji::find([](auto& h) { return on(h, "localhost:3"); }), aji::Errc::no_match);
    CHECK_ERRC(aji::find([](auto& h) { return h.is_dummy(); }), aji::Errc::ambiguous_match);
}

void lock_reports_failure()
{
    write_config("");
    CHECK(aji::add_remote_server("localhost:1", "pw", aji::Persist::temporary));
    auto chain = aji::find([](auto&) { return true; });
    CHECK(chain);
    if (!chain)
        return;
    // A dummy chain has no server-side id, so aji_lock_chain refuses it.
    auto cl = chain->lock(100ms);
    CHECK_ERRC(cl, aji::Errc::invalid_chain_id);
    if (!cl)
        CHECK(std::string_view(cl.error().call) == "aji_lock_chain");
}

// Every call on a lock object that holds nothing reports it, instead of
// reaching the library. (Locks that *were* held need hardware to test.)
void empty_objects_report_empty_lock()
{
    aji::ChainLock cl;   // empty
    CHECK(!cl);
    CHECK_ERRC(cl.taps(), aji::Errc::empty_lock);
    CHECK_ERRC(cl.nodes(0), aji::Errc::empty_lock);
    CHECK_ERRC(cl.parameter("JtagClock"), aji::Errc::empty_lock);
    CHECK_ERRC(cl.open_tap(0, {}, "test"), aji::Errc::empty_lock);
    CHECK_ERRC(cl.unlock(), aji::Errc::empty_lock);

    aji::Open<aji::Kind::tap> open;   // empty
    CHECK(!open);
    CHECK_ERRC(cl.hand_to(open, aji::Pack::never), aji::Errc::empty_lock);   // the lock is checked first
    CHECK_ERRC(open.lock(1ms, aji::Pack::never), aji::Errc::invalid_open_id);

    aji::Session<aji::Kind::tap> s;   // empty
    CHECK(!s);
    auto ir = s.ir(6);
    CHECK(ir.ready());
    CHECK_ERRC(ir.get(), aji::Errc::empty_lock);
    CHECK_ERRC(s.dr({.length = 32, .read_length = 32}).get(), aji::Errc::empty_lock);
    CHECK_ERRC(s.flush(), aji::Errc::empty_lock);
    CHECK_ERRC(s.unlock(), aji::Errc::empty_lock);
    CHECK_ERRC(s.hand_back(), aji::Errc::empty_lock);
    CHECK_ERRC(s.hand_to(open), aji::Errc::empty_lock);

    // Moving keeps "empty" empty, and leaves the source empty.
    aji::ChainLock moved = std::move(cl);
    CHECK(!moved && !cl);
    aji::Session<aji::Kind::tap> s2 = std::move(s);
    CHECK(!s2 && !s);
    CHECK(aji::detail::Pin::count() == 0);   // empty locks pin nothing
}

// tests/ub/05 with the wrapper: a rescan that reallocates the library's
// strings can't reach us, and the copied HardwareInfo stays valid.
void info_survives_library_realloc()
{
    write_config("");
    CHECK(aji::add_remote_server("localhost:1", "pw", aji::Persist::temporary));
    aji::HardwareInfo kept;
    {
        auto first = aji::scan(1ms);   // "Connecting ..."
        CHECK(first && !first->empty());
        if (first && !first->empty())
            kept = first->front().info();
    }
    auto second = aji::scan(2s);       // "Unable to connect": library frees its old strings
    CHECK(second);
    CHECK(kept.server && *kept.server == "localhost:1");   // ASan would flag a dangling read here
}

// tests/ub/06 with the wrapper: another process removes a server from the
// shared config; while we hold its chain, our management calls refuse to
// run, so the library can't free the chain under us.
void config_edit_by_other_process_is_harmless()
{
    write_config(remote(1, "localhost:1") + remote(2, "localhost:2"));
    {
        auto chain = aji::find([](auto& h) { return on(h, "localhost:2"); });
        CHECK(chain);
        if (!chain)
            return;

        sleep(1);   // the config file's mtime has one-second resolution
        if (fork() == 0) {
            write_config(remote(1, "localhost:1"));   // `jtagconfig --removeserver` elsewhere
            _exit(0);
        }
        wait(nullptr);

        CHECK_ERRC(aji::defined_devices(), aji::Errc::chains_in_use);   // would have freed `chain`
        auto taps = chain->cached_taps();   // still a live object: an ordinary error, not a use-after-free
        CHECK(!taps);
    }   // handle dropped

    auto after = aji::scan(2s);   // now the library may drop localhost:2
    CHECK(after);
    if (after)
        for (auto& c : *after)
            CHECK(!on(c.info(), "localhost:2"));
}

void error_messages()
{
    aji::Error e{aji::Errc::chains_in_use, "aji::scan", {}};
    CHECK(e.message() == "aji::scan: chains_in_use");
    CHECK(e.error_code() == aji::Errc::chains_in_use);
    CHECK(std::string(e.error_code().category().name()) == "aji");
    aji::Error with_info{aji::Errc::timeout, "aji_lock", "busy"};
    CHECK(with_info.message() == "aji_lock: timeout (busy)");
}

void output_callback_registers()
{
    aji::on_output([](aji::OutputLevel, std::string_view) {});
    aji::on_output({});
}

const std::map<std::string_view, std::function<void()>> cases = {
    {"scan_copies_info", scan_copies_info},
    {"management_refused_while_chains_alive", management_refused_while_chains_alive},
    {"copies_and_moves_pin", copies_and_moves_pin},
    {"find_matches_exactly_one", find_matches_exactly_one},
    {"lock_reports_failure", lock_reports_failure},
    {"empty_objects_report_empty_lock", empty_objects_report_empty_lock},
    {"info_survives_library_realloc", info_survives_library_realloc},
    {"config_edit_by_other_process_is_harmless", config_edit_by_other_process_is_harmless},
    {"error_messages", error_messages},
    {"output_callback_registers", output_callback_registers},
};

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2 || !cases.contains(argv[1])) {
        std::fputs("usage: aji_no_hardware <case>\ncases:\n", stderr);
        for (auto& [name, fn] : cases)
            std::fprintf(stderr, "  %.*s\n", int(name.size()), name.data());
        return 2;
    }
    cases.at(argv[1])();
    return failures == 0 ? 0 : 1;
}
