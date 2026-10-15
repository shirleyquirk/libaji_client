#include "c_api.hpp"   // first: aji.h must precede every other header

#include "aji/chain.hpp"


#include <utility>

namespace aji {

Chain::Chain(Chain&& other) noexcept
    : id_(std::exchange(other.id_, nullptr)), info_(std::move(other.info_)), pin_(std::move(other.pin_))
{}

Chain& Chain::operator=(Chain&& other) noexcept
{
    if (this != &other) {
        id_ = std::exchange(other.id_, nullptr);
        info_ = std::move(other.info_);
        pin_ = std::move(other.pin_);
    }
    return *this;
}

expected<std::vector<TapInfo>> Chain::cached_taps() const
{
    if (id_ == nullptr)
        return unexpected(Error{Errc::invalid_chain_id, "Chain::cached_taps", {}});
    std::vector<AJI_DEVICE> devices;
    DWORD count = 0;
    for (;;) {
        DWORD space = static_cast<DWORD>(devices.size());
        count = space;
        AJI_ERROR e = aji_read_device_chain(id_, &count, devices.empty() ? nullptr : devices.data(), false);
        if (e == AJI_TOO_MANY_DEVICES || (e == AJI_NO_ERROR && count > space)) {
            devices.resize(count);
            continue;
        }
        if (e != AJI_NO_ERROR)
            return unexpected(detail::from_aji(e, "aji_read_device_chain"));
        break;
    }
    std::vector<TapInfo> out;
    for (DWORD i = 0; i < count; ++i)
        out.push_back(detail::tap_from(devices[i], i));
    return out;
}

expected<void> Chain::lock(std::chrono::milliseconds timeout) const
{
    if (id_ == nullptr)
        return unexpected(Error{Errc::invalid_chain_id, "Chain::locked", {}});
    return detail::check(aji_lock_chain(id_, static_cast<DWORD>(timeout.count())), "aji_lock_chain");
}

} // namespace aji
