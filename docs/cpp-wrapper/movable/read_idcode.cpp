// cpp/examples/read_idcode.cpp, written against the movable sketch.
#include "sketch.hpp"

#include <cstdio>
#include <string>

using namespace std::chrono_literals;

aji::expected<aji::Bits> read_idcode(const aji2::Chain& chain, std::uint32_t position, std::uint32_t idcode_ir)
{
    auto cl = chain.lock(10s);
    if (!cl)
        return aji::unexpected(cl.error());

    auto tap = cl->open_tap(position, aji::ClaimSet{}.add(aji::ClaimType::ir_shared, idcode_ir), "read_idcode");
    if (!tap)
        return aji::unexpected(tap.error());

    auto s = cl->hand_to(*tap, aji::Pack::never);   // *cl is empty from here on
    if (!s)
        return aji::unexpected(s.error());
    (void)s->ir(idcode_ir);
    auto idcode = s->dr({.length = 32, .read_length = 32});

    if (auto f = s->flush(); !f)                    // or just let ~Session flush and unlock
        return aji::unexpected(f.error());
    return idcode.get();
}   // ~idcode; ~s: unlock (the lock goes to nobody); ~tap: aji_close_device; ~cl: empty, nothing to do

int run(const aji2::Chain& chain)
{
    auto id = read_idcode(chain, 0, 6);
    if (!id) {
        std::fprintf(stderr, "%s\n", id.error().message().c_str());
        return 1;
    }
    std::printf("IDCODE %02X%02X%02X%02X\n", id->bytes[3], id->bytes[2], id->bytes[1], id->bytes[0]);
    return 0;
}
