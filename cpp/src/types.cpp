#include "aji/types.hpp"

namespace aji {

std::string HardwareInfo::display_name() const
{
    std::string out = hw_name.empty() ? std::string(device_name.value_or("(dummy)")) : hw_name;
    if (server)
        out += " on " + *server;
    if (port)
        out += " [" + *port + "]";
    return out;
}

} // namespace aji
