// Management functions. Each one may make the library free chain objects,
// their strings or server connections (FINDINGS.md §2.4), so each refuses to
// run while any Chain or Open is alive, and they are serialised against each
// other.
#include "c_api.hpp"   // first: aji.h must precede every other header

#include "aji/management.hpp"


#include <mutex>

namespace aji {

namespace detail {

struct ChainAccess {
    static Chain      make(AJI_CHAIN* id, HardwareInfo info) { return Chain(id, std::move(info)); }
    static AJI_CHAIN* id(const Chain& c) { return c.id_; }
};

} // namespace detail

namespace {

using detail::check;
using detail::from_aji;

std::mutex& management_mutex()
{
    static std::mutex m;
    return m;
}

// Holds the management mutex for one call, if no handle is alive (beyond
// `allowed_pins` that the call itself holds).
expected<std::unique_lock<std::mutex>> enter(const char* call, long allowed_pins = 0)
{
    std::unique_lock lock(management_mutex());
    if (detail::Pin::count() > allowed_pins)
        return unexpected(Error{Errc::chains_in_use, call, {}});
    return lock;
}

DWORD ms(std::chrono::milliseconds t) { return static_cast<DWORD>(t.count()); }

} // namespace

// ---------------------------------------------------------------------------
// Finding hardware

expected<std::vector<Chain>> scan(std::chrono::milliseconds timeout)
{
    auto guard = enter("aji::scan");
    if (!guard)
        return unexpected(guard.error());

    if (timeout.count() < 1)
        timeout = std::chrono::milliseconds(1);   // FINDINGS.md §4: timeout 0 can crash

    std::vector<AJI_HARDWARE> hw(8);
    std::vector<char*>        versions(8, nullptr);
    DWORD count = 0;
    for (;;) {
        count = static_cast<DWORD>(hw.size());
        std::fill(versions.begin(), versions.end(), nullptr);
        AJI_ERROR e = aji_get_hardware2(&count, hw.data(), versions.data(), ms(timeout));
        if (e == AJI_TOO_MANY_DEVICES) {
            for (char* v : versions)
                delete[] v;
            hw.resize(count + 4);
            versions.resize(count + 4);
            continue;
        }
        // AJI_TIMEOUT: some servers are still connecting. The list is valid;
        // those servers appear as dummy entries.
        if (e != AJI_NO_ERROR && e != AJI_TIMEOUT) {
            for (char* v : versions)
                delete[] v;
            return unexpected(from_aji(e, "aji_get_hardware2"));
        }
        break;
    }

    std::vector<Chain> out;
    out.reserve(count);
    for (DWORD i = 0; i < count; ++i)
        out.push_back(detail::ChainAccess::make(hw[i].chain_id, detail::info_from(hw[i], versions[i])));
    for (char* v : versions)
        delete[] v;   // caller-owned (jtag_client_chain.cpp:150, :155)
    return out;
}

namespace {

expected<Chain> found(AJI_ERROR e, const AJI_HARDWARE& hw, const char* call, Errc on_failure)
{
    if (e == AJI_FAILURE)
        return unexpected(Error{on_failure, call, {}});
    if (e == AJI_NO_DEVICES)
        return unexpected(Error{Errc::no_match, call, {}});
    if (e != AJI_NO_ERROR)
        return unexpected(from_aji(e, call));
    return detail::ChainAccess::make(hw.chain_id, detail::info_from(hw, nullptr));
}

} // namespace

expected<Chain> find_by_name(std::string_view name, std::chrono::milliseconds timeout)
{
    auto guard = enter("aji::find_by_name");
    if (!guard)
        return unexpected(guard.error());
    const std::string n(name);
    AJI_HARDWARE hw{};
    // AJI_FAILURE: more than one cable matches (aji.h:614).
    return found(aji_find_hardware(n.c_str(), &hw, ms(timeout)), hw, "aji_find_hardware", Errc::ambiguous_match);
}

expected<Chain> find_by_persistent_id(std::uint32_t id, std::chrono::milliseconds timeout)
{
    auto guard = enter("aji::find_by_persistent_id");
    if (!guard)
        return unexpected(guard.error());
    AJI_HARDWARE hw{};
    // AJI_FAILURE: nothing has this id (aji.h:592).
    return found(aji_find_hardware(id, &hw, ms(timeout)), hw, "aji_find_hardware", Errc::no_match);
}

// ---------------------------------------------------------------------------
// Changing the hardware list

expected<void> add_hardware(const HardwareSpec& spec)
{
    auto guard = enter("aji::add_hardware");
    if (!guard)
        return unexpected(guard.error());
    AJI_HARDWARE hw{};
    hw.hw_name = spec.hw_name.c_str();
    hw.port = spec.port ? spec.port->c_str() : nullptr;
    hw.device_name = spec.device_name ? spec.device_name->c_str() : nullptr;
    return check(aji_add_hardware(&hw), "aji_add_hardware");
}

expected<void> remove(Chain chain)
{
    // `chain` itself holds one pin.
    auto guard = enter("aji::remove", 1);
    if (!guard)
        return unexpected(guard.error());
    if (!chain)
        return unexpected(Error{Errc::invalid_chain_id, "aji::remove", {}});
    return check(aji_remove_hardware(detail::ChainAccess::id(chain)), "aji_remove_hardware");
}

expected<void> add_remote_server(const std::string& host, const std::string& password, Persist persist)
{
    auto guard = enter("aji::add_remote_server");
    if (!guard)
        return unexpected(guard.error());
    return check(aji_add_remote_server(host.c_str(), password.c_str(), persist == Persist::temporary),
                 "aji_add_remote_server");
}

expected<void> enable_remote_clients(bool enable, const std::string& password)
{
    auto guard = enter("aji::enable_remote_clients");
    if (!guard)
        return unexpected(guard.error());
    return check(aji_enable_remote_clients(enable, password.c_str()), "aji_enable_remote_clients");
}

expected<bool> remote_clients_enabled()
{
    auto guard = enter("aji::remote_clients_enabled");
    if (!guard)
        return unexpected(guard.error());
    bool enabled = false;
    if (auto e = check(aji_get_remote_clients_enabled(&enabled), "aji_get_remote_clients_enabled"); !e)
        return unexpected(e.error());
    return enabled;
}

// ---------------------------------------------------------------------------
// User device database

expected<void> define_device(const DeviceSpec& device)
{
    auto guard = enter("aji::define_device");
    if (!guard)
        return unexpected(guard.error());
    AJI_DEVICE d = detail::device_to(device);
    return check(aji_define_device(&d), "aji_define_device");
}

expected<void> undefine_device(const DeviceSpec& device)
{
    auto guard = enter("aji::undefine_device");
    if (!guard)
        return unexpected(guard.error());
    AJI_DEVICE d = detail::device_to(device);
    return check(aji_undefine_device(&d), "aji_undefine_device");
}

expected<std::vector<DeviceSpec>> defined_devices()
{
    auto guard = enter("aji::defined_devices");
    if (!guard)
        return unexpected(guard.error());
    std::vector<AJI_DEVICE> devices;
    DWORD count = 0;
    for (;;) {
        DWORD space = static_cast<DWORD>(devices.size());
        count = space;
        AJI_ERROR e = aji_get_defined_devices(&count, devices.empty() ? nullptr : devices.data());
        if (e == AJI_TOO_MANY_DEVICES || (e == AJI_NO_ERROR && count > space)) {
            devices.resize(count);
            continue;
        }
        if (e != AJI_NO_ERROR)
            return unexpected(from_aji(e, "aji_get_defined_devices"));
        break;
    }
    std::vector<DeviceSpec> out;
    for (DWORD i = 0; i < count; ++i)
        out.push_back(detail::device_from(devices[i]));
    return out;
}

// ---------------------------------------------------------------------------
// Not guarded

namespace {

std::mutex& output_mutex()
{
    static std::mutex m;
    return m;
}

std::function<void(OutputLevel, std::string_view)>& output_fn()
{
    static std::function<void(OutputLevel, std::string_view)> fn;
    return fn;
}

void output_trampoline(void*, DWORD level, const char* line)
{
    std::lock_guard lock(output_mutex());
    if (output_fn())
        output_fn()(static_cast<OutputLevel>(level), line ? std::string_view(line) : std::string_view());
}

} // namespace

void on_output(std::function<void(OutputLevel, std::string_view)> fn)
{
    std::lock_guard lock(output_mutex());
    output_fn() = std::move(fn);
    aji_register_output_callback(output_fn() ? &output_trampoline : nullptr, nullptr);
}

std::string last_error_info()
{
    const char* info = aji_get_error_info();
    return info ? info : "";
}

} // namespace aji
