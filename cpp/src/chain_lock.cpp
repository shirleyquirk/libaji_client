#include "c_api.hpp"   // first: aji.h must precede every other header

#include "aji/chain_lock.hpp"

#include <exception>
#include <utility>


namespace aji {

using detail::check;
using detail::from_aji;

ChainLock::ChainLock(detail::ChainSeed seed)
    : seed_(std::move(seed)), pin_(std::in_place), owner_(std::this_thread::get_id()), held_(true)
{}

ChainLock::ChainLock(ChainLock&& other) noexcept
    : seed_(std::move(other.seed_)), pin_(std::move(other.pin_)), owner_(other.owner_),
      held_(std::exchange(other.held_, false))
{
    other.pin_.reset();
}

ChainLock& ChainLock::operator=(ChainLock&& other) noexcept
{
    if (this != &other) {
        release();
        seed_ = std::move(other.seed_);
        pin_ = std::move(other.pin_);
        other.pin_.reset();
        owner_ = other.owner_;
        held_ = std::exchange(other.held_, false);
    }
    return *this;
}

ChainLock::~ChainLock() { release(); }

void ChainLock::release() noexcept
{
    if (!held_)
        return;
    if (owner_ != std::this_thread::get_id())
        std::terminate();   // the library's mutex belongs to the owner thread; see chain_lock.hpp
    aji_unlock_chain(seed_.id);
    held_ = false;
    pin_.reset();
}

expected<void> ChainLock::unlock()
{
    if (auto u = usable("ChainLock::unlock"); !u)
        return u;
    AJI_ERROR e = aji_unlock_chain(seed_.id);   // assumes unlocked whatever happens (jtag_client_chain.cpp:880-884)
    held_ = false;
    pin_.reset();
    return check(e, "aji_unlock_chain");
}

expected<void> ChainLock::usable(const char* call) const { return detail::check_lock(held_, owner_, call); }

expected<std::unique_ptr<detail::SessionCore>> ChainLock::hand_to_core(detail::OpenState* open, Pack pack)
{
    if (auto u = usable("ChainLock::hand_to"); !u)
        return unexpected(u.error());
    if (open == nullptr)
        return unexpected(Error{Errc::invalid_open_id, "ChainLock::hand_to", {}});
    if (open->in_session)
        return unexpected(Error{Errc::locked, "ChainLock::hand_to", {}});
    if (open->chain != seed_.id)
        return unexpected(Error{Errc::invalid_parameter, "ChainLock::hand_to: open is on another chain", {}});

    AJI_ERROR e = aji_unlock_chain_lock(seed_.id, open->id, static_cast<AJI_PACK_STYLE>(pack));
    if (e != AJI_NO_ERROR) {
        const Error first = from_aji(e, "aji_unlock_chain_lock");
        // Failures before sending leave the chain locked; a failure after
        // sending leaves nobody holding the lock (jtag_client_open.cpp:544).
        // Re-locking tells them apart: aji_lock_chain returns AJI_LOCKED if we
        // still hold it (jtag_client_chain.cpp:829-830).
        AJI_ERROR relock = aji_lock_chain(seed_.id, static_cast<DWORD>(seed_.relock_timeout.count()));
        if (relock == AJI_NO_ERROR || relock == AJI_LOCKED)
            return unexpected(first);
        held_ = false;
        pin_.reset();
        return unexpected(Error{Errc::lock_lost, "ChainLock::hand_to", first.message()});
    }
    held_ = false;
    pin_.reset();
    return std::make_unique<detail::SessionCore>(*open, pack, seed_, seed_.relock_timeout);
}

std::unique_ptr<detail::OpenState> ChainLock::adopt(AJI_OPEN* id, AJI_CHAIN* chain)
{
    auto state = std::make_unique<detail::OpenState>();
    state->id = id;
    state->chain = chain;
    return state;
}

expected<std::vector<TapInfo>> ChainLock::taps(bool auto_scan)
{
    if (auto u = usable("ChainLock::taps"); !u)
        return unexpected(u.error());
    DWORD count = 0;
    std::vector<AJI_DEVICE> devices;
    for (;;) {
        DWORD space = static_cast<DWORD>(devices.size());
        count = space;
        AJI_ERROR e = aji_read_device_chain(seed_.id, &count, devices.empty() ? nullptr : devices.data(), auto_scan);
        if (e == AJI_TOO_MANY_DEVICES || (e == AJI_NO_ERROR && count > space)) {
            devices.resize(count);
            continue;
        }
        if (e != AJI_NO_ERROR)
            return unexpected(from_aji(e, "aji_read_device_chain"));
        break;
    }
    std::vector<TapInfo> out;
    for (DWORD i = 0; i < count; ++i)
        out.push_back(detail::tap_from(devices[i], i));
    return out;
}

expected<std::vector<SldNodeInfo>> ChainLock::nodes(std::uint32_t tap_position)
{
    if (auto u = usable("ChainLock::nodes"); !u)
        return unexpected(u.error());
    std::vector<AJI_HIER_ID>  ids(16);
    std::vector<AJI_HUB_INFO> hubs(16);
    DWORD n = 0;
    for (;;) {
        n = static_cast<DWORD>(ids.size());
        // The overload with a trailing int works around wrong idcodes seen
        // with the plain one (aji.h:1820-1835).
        AJI_ERROR e = aji_get_nodes(seed_.id, tap_position, ids.data(), &n, hubs.data(), 0);
        if (e == AJI_TOO_MANY_DEVICES) {
            ids.resize(n);
            hubs.resize(n);
            continue;
        }
        if (e != AJI_NO_ERROR)
            return unexpected(from_aji(e, "aji_get_nodes"));
        break;
    }
    std::vector<SldNodeInfo> out;
    for (DWORD i = 0; i < n; ++i)
        out.push_back(SldNodeInfo{detail::hier_from(ids[i]), detail::hub_from(hubs[i])});
    return out;
}

expected<std::uint32_t> ChainLock::parameter(const std::string& name)
{
    if (auto u = usable("ChainLock::parameter"); !u)
        return unexpected(u.error());
    DWORD value = 0;
    if (auto e = check(aji_get_parameter(seed_.id, name.c_str(), &value), "aji_get_parameter"); !e)
        return unexpected(e.error());
    return value;
}

expected<void> ChainLock::set_parameter(const std::string& name, std::uint32_t value)
{
    if (auto u = usable("ChainLock::set_parameter"); !u)
        return u;
    return check(aji_set_parameter(seed_.id, name.c_str(), value), "aji_set_parameter");
}

expected<std::vector<std::uint8_t>> ChainLock::parameter_block(const std::string& name,
                                                              std::span<const std::uint8_t> send)
{
    if (auto u = usable("ChainLock::parameter_block"); !u)
        return unexpected(u.error());
    if (send.size() > AJI_PARAMETER_MAX)
        return unexpected(Error{Errc::invalid_parameter, "ChainLock::parameter_block", {}});
    std::vector<std::uint8_t> buffer(AJI_PARAMETER_MAX);
    std::copy(send.begin(), send.end(), buffer.begin());
    DWORD size = AJI_PARAMETER_MAX;
    if (auto e = check(aji_get_parameter(seed_.id, name.c_str(), buffer.data(), &size, static_cast<DWORD>(send.size())),
                       "aji_get_parameter");
        !e)
        return unexpected(e.error());
    buffer.resize(size);
    return buffer;
}

expected<Open<Kind::tap>> ChainLock::open_tap(std::uint32_t tap_position, const ClaimSet& claims,
                                              const std::string& application)
{
    if (auto u = usable("ChainLock::open_tap"); !u)
        return unexpected(u.error());
    auto c = detail::claims_to(claims);
    AJI_OPEN_ID id = nullptr;
    if (auto e = check(aji_open_device(seed_.id, tap_position, &id, c.data(), static_cast<DWORD>(c.size()),
                                       application.c_str()),
                       "aji_open_device");
        !e)
        return unexpected(e.error());
    return Open<Kind::tap>(adopt(id, seed_.id));
}

expected<Open<Kind::sld_node>> ChainLock::open_node(std::uint32_t tap_position, const SldNodeInfo& node,
                                                    const ClaimSet& claims, const std::string& application)
{
    if (auto u = usable("ChainLock::open_node"); !u)
        return unexpected(u.error());
    auto c = detail::claims_to(claims);
    AJI_HIER_ID hier = detail::hier_to(node.id);
    AJI_OPEN_ID id = nullptr;
    if (auto e = check(aji_open_node(seed_.id, tap_position, &hier, &id, c.data(), static_cast<DWORD>(c.size()),
                                     application.c_str()),
                       "aji_open_node");
        !e)
        return unexpected(e.error());
    return Open<Kind::sld_node>(adopt(id, seed_.id));
}

expected<Open<Kind::sld_hub>> ChainLock::open_hub(std::uint32_t tap_position, const SldHubAddr& hub,
                                                  const ClaimSet& claims, const std::string& application)
{
    if (auto u = usable("ChainLock::open_hub"); !u)
        return unexpected(u.error());
    auto c = detail::claims_to(claims);
    AJI_HIER_ID hier = detail::hier_to(hub.node);
    AJI_OPEN_ID id = nullptr;
    if (auto e = check(aji_open_hub(seed_.id, tap_position, hub.level, &hier, &id, c.data(),
                                    static_cast<DWORD>(c.size()), application.c_str()),
                       "aji_open_hub");
        !e)
        return unexpected(e.error());
    return Open<Kind::sld_hub>(adopt(id, seed_.id));
}

expected<Open<Kind::entire_chain>> ChainLock::open_entire(ChainType style, const std::string& application)
{
    if (auto u = usable("ChainLock::open_entire"); !u)
        return unexpected(u.error());
    AJI_OPEN_ID id = nullptr;
    if (auto e = check(aji_open_entire_device_chain(seed_.id, &id, static_cast<AJI_CHAIN_TYPE>(style),
                                                    application.c_str()),
                       "aji_open_entire_device_chain");
        !e)
        return unexpected(e.error());
    return Open<Kind::entire_chain>(adopt(id, seed_.id));
}

} // namespace aji
