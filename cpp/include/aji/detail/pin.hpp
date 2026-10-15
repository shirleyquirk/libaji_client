// Process-wide count of live handles (Chain, Open) that point into
// library-owned objects. Management functions refuse to run while it is
// non-zero, because they can free those objects (FINDINGS.md §2.4).
#pragma once

namespace aji::detail {

class Pin {
public:
    Pin() noexcept;
    Pin(const Pin&) noexcept;
    Pin(Pin&& other) noexcept;              // `other` stops counting at once
    Pin& operator=(const Pin& other) noexcept;
    Pin& operator=(Pin&& other) noexcept;
    ~Pin();

    void reset() noexcept { release(); }   // stop counting (the handle no longer points anywhere)

    static long count() noexcept;

private:
    void acquire() noexcept;
    void release() noexcept;
    bool active_ = false;
};

} // namespace aji::detail
