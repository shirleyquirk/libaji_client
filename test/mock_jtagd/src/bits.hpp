// LSB-first bit vector, matching the AJI buffer convention (bit 0 = first bit shifted).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mock_jtagd {

class Bits {
public:
    Bits() = default;
    explicit Bits(std::size_t n) : bits_(n, false) {}

    static Bits from_bytes(std::span<const std::uint8_t> bytes, std::size_t n)
    {
        Bits b(n);
        for (std::size_t i = 0; i < n && i / 8 < bytes.size(); ++i)
            b.bits_[i] = (bytes[i / 8] >> (i % 8)) & 1;
        return b;
    }

    static Bits from_u64(std::uint64_t v, std::size_t n)
    {
        Bits b(n);
        for (std::size_t i = 0; i < n && i < 64; ++i)
            b.bits_[i] = (v >> i) & 1;
        return b;
    }

    std::vector<std::uint8_t> to_bytes() const
    {
        std::vector<std::uint8_t> out((bits_.size() + 7) / 8, 0);
        for (std::size_t i = 0; i < bits_.size(); ++i)
            if (bits_[i])
                out[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
        return out;
    }

    std::uint64_t to_u64() const
    {
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < bits_.size() && i < 64; ++i)
            if (bits_[i])
                v |= std::uint64_t{1} << i;
        return v;
    }

    Bits slice(std::size_t offset, std::size_t n) const
    {
        Bits b(n);
        for (std::size_t i = 0; i < n && offset + i < bits_.size(); ++i)
            b.bits_[i] = bits_[offset + i];
        return b;
    }

    void splice(std::size_t offset, const Bits & src)
    {
        for (std::size_t i = 0; i < src.size() && offset + i < bits_.size(); ++i)
            bits_[offset + i] = src.bits_[i];
    }

    void set(std::size_t i, bool v) { bits_[i] = v; }

    void append(const Bits & src) { bits_.insert(bits_.end(), src.bits_.begin(), src.bits_.end()); }

    void resize(std::size_t n) { bits_.resize(n, false); }
    std::size_t size() const { return bits_.size(); }
    bool operator[](std::size_t i) const { return bits_[i]; }
    bool operator==(const Bits &) const = default;

private:
    std::vector<bool> bits_;
};

} // namespace mock_jtagd
