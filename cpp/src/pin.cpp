#include "aji/detail/pin.hpp"

#include <atomic>

namespace aji::detail {

namespace {
std::atomic<long> live_pins{0};
}

Pin::Pin() noexcept { acquire(); }
// A pin is active while its owner holds a handle. Copying copies that
// state; moving transfers it.
Pin::Pin(const Pin& other) noexcept { if (other.active_) acquire(); }
Pin::Pin(Pin&& other) noexcept { if (other.active_) { acquire(); other.release(); } }

Pin& Pin::operator=(const Pin& other) noexcept
{
    if (other.active_) acquire(); else release();
    return *this;
}

Pin& Pin::operator=(Pin&& other) noexcept
{
    if (this != &other) {
        if (other.active_) { acquire(); other.release(); } else release();
    }
    return *this;
}

Pin::~Pin() { release(); }

long Pin::count() noexcept { return live_pins.load(); }

void Pin::acquire() noexcept
{
    if (!active_) {
        active_ = true;
        ++live_pins;
    }
}

void Pin::release() noexcept
{
    if (active_) {
        active_ = false;
        --live_pins;
    }
}

} // namespace aji::detail
