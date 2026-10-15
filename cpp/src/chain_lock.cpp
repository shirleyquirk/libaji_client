#include "c_api.hpp"   // first: aji.h must precede every other header

#include "aji/chain_lock.hpp"


namespace aji {

using detail::check;
using detail::from_aji;

ChainLock::~ChainLock()
{
    if (lost_) {
        if (!out_.error)
            out_.error = Error{Errc::lock_lost, "ChainLock", {}};
        return;
    }
    if (AJI_ERROR e = aji_unlock_chain(id_); e != AJI_NO_ERROR && !out_.error)
        out_.error = from_aji(e, "aji_unlock_chain");
}

expected<void> ChainLock::usable(const char* call) const
{
    if (lost_)
        return unexpected(Error{Errc::lock_lost, call, {}});
    if (handed_away_)
        return unexpected(Error{Errc::lock_handed_away, call, {}});
    return {};
}

expected<void> ChainLock::hand_to(detail::OpenState& open, Pack pack)
{
    if (auto u = usable("ChainLock::with"); !u)
        return u;
    if (open.in_session)
        return unexpected(Error{Errc::locked, "ChainLock::with", {}});
    AJI_ERROR e = aji_unlock_chain_lock(id_, open.id, static_cast<AJI_PACK_STYLE>(pack));
    if (e != AJI_NO_ERROR) {
        Error err = from_aji(e, "aji_unlock_chain_lock");
        // Failures before sending leave the chain locked; a failure after
        // sending leaves nobody holding the lock (jtag_client_open.cpp:544).
        // Re-locking tells them apart: aji_lock_chain returns AJI_LOCKED if we
        // still hold it (jtag_client_chain.cpp:829-830).
        AJI_ERROR relock = aji_lock_chain(id_, static_cast<DWORD>(relock_timeout_.count()));
        if (relock != AJI_NO_ERROR && relock != AJI_LOCKED)
            lost_ = true;
        return unexpected(err);
    }
    handed_away_ = true;
    return {};
}

std::unique_ptr<detail::OpenState> ChainLock::adopt(AJI_OPEN* id)
{
    auto state = std::make_unique<detail::OpenState>();
    state->id = id;
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
        AJI_ERROR e = aji_read_device_chain(id_, &count, devices.empty() ? nullptr : devices.data(), auto_scan);
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
        AJI_ERROR e = aji_get_nodes(id_, tap_position, ids.data(), &n, hubs.data(), 0);
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
    if (auto e = check(aji_get_parameter(id_, name.c_str(), &value), "aji_get_parameter"); !e)
        return unexpected(e.error());
    return value;
}

expected<void> ChainLock::set_parameter(const std::string& name, std::uint32_t value)
{
    if (auto u = usable("ChainLock::set_parameter"); !u)
        return u;
    return check(aji_set_parameter(id_, name.c_str(), value), "aji_set_parameter");
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
    if (auto e = check(aji_get_parameter(id_, name.c_str(), buffer.data(), &size, static_cast<DWORD>(send.size())),
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
    if (auto e = check(aji_open_device(id_, tap_position, &id, c.data(), static_cast<DWORD>(c.size()),
                                       application.c_str()),
                       "aji_open_device");
        !e)
        return unexpected(e.error());
    return Open<Kind::tap>(adopt(id));
}

expected<Open<Kind::sld_node>> ChainLock::open_node(std::uint32_t tap_position, const SldNodeInfo& node,
                                                    const ClaimSet& claims, const std::string& application)
{
    if (auto u = usable("ChainLock::open_node"); !u)
        return unexpected(u.error());
    auto c = detail::claims_to(claims);
    AJI_HIER_ID hier = detail::hier_to(node.id);
    AJI_OPEN_ID id = nullptr;
    if (auto e = check(aji_open_node(id_, tap_position, &hier, &id, c.data(), static_cast<DWORD>(c.size()),
                                     application.c_str()),
                       "aji_open_node");
        !e)
        return unexpected(e.error());
    return Open<Kind::sld_node>(adopt(id));
}

expected<Open<Kind::sld_hub>> ChainLock::open_hub(std::uint32_t tap_position, const SldHubAddr& hub,
                                                  const ClaimSet& claims, const std::string& application)
{
    if (auto u = usable("ChainLock::open_hub"); !u)
        return unexpected(u.error());
    auto c = detail::claims_to(claims);
    AJI_HIER_ID hier = detail::hier_to(hub.node);
    AJI_OPEN_ID id = nullptr;
    if (auto e = check(aji_open_hub(id_, tap_position, hub.level, &hier, &id, c.data(),
                                    static_cast<DWORD>(c.size()), application.c_str()),
                       "aji_open_hub");
        !e)
        return unexpected(e.error());
    return Open<Kind::sld_hub>(adopt(id));
}

expected<Open<Kind::entire_chain>> ChainLock::open_entire(ChainType style, const std::string& application)
{
    if (auto u = usable("ChainLock::open_entire"); !u)
        return unexpected(u.error());
    AJI_OPEN_ID id = nullptr;
    if (auto e = check(aji_open_entire_device_chain(id_, &id, static_cast<AJI_CHAIN_TYPE>(style),
                                                    application.c_str()),
                       "aji_open_entire_device_chain");
        !e)
        return unexpected(e.error());
    return Open<Kind::entire_chain>(adopt(id));
}

} // namespace aji
