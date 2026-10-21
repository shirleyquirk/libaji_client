// Minimal RFC 1321 MD5, used only for the jtagd challenge/response handshake.
// Deliberately independent of the client's jtag_md5_pd so the two can't share a bug.
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace mock_jtagd {

std::array<std::uint8_t, 16> md5(std::span<const std::uint8_t> data);

} // namespace mock_jtagd
