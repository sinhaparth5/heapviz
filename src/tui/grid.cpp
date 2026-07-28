/* heapviz - address space to grid bucketizer (M3.1).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tui/grid.h"

#include <bit>
#include <cstdio>
#include <cstring>

namespace hv {

namespace {

/* Smallest power of two at or above `v`, clamped into the legal cell range.
 *
 * std::bit_ceil is undefined when the result would not fit, so the clamp has to
 * happen before the call, not after. It cannot fire in practice given
 * kMaxCellBytes is 2^30, but "cannot happen given the current constant" is
 * exactly the reasoning that rots when someone raises the constant. */
std::uint64_t clamped_pow2(std::uint64_t v) noexcept {
    if (v <= kMinCellBytes) return kMinCellBytes;
    if (v >= kMaxCellBytes) return kMaxCellBytes;
    return std::bit_ceil(v);
}

const char *const kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};

/* Splits `bytes` into a value and a unit, dividing by 1024 until the value
 * fits in three digits. Returns the unit index and writes the scaled value. */
std::size_t scale_bytes(std::uint64_t bytes, std::uint64_t &whole,
                        std::uint64_t &frac_tenths) noexcept {
    std::size_t unit = 0;
    std::uint64_t v = bytes;
    std::uint64_t rem = 0;
    const std::size_t last = (sizeof kUnits / sizeof kUnits[0]) - 1;

    while (v >= 1024 && unit < last) {
        rem = v % 1024;
        v /= 1024;
        ++unit;
    }
    whole = v;
    /* One decimal, but only below ten, where it is the difference between
     * "1 KB" and "1.5 KB". Above that the digit is noise in a label the user
     * reads at a glance. */
    frac_tenths = (unit > 0 && v < 10) ? (rem * 10) / 1024 : 0;
    return unit;
}

} // namespace

bool Grid::configure(std::uint64_t base, std::uint64_t end, int cols,
                     int rows) noexcept {
    base_  = base;
    end_   = end;
    cols_  = cols > 0 ? cols : 0;
    rows_  = rows > 0 ? rows : 0;
    valid_ = false;

    /* Every degenerate case lands here, and they are all reachable: an inverted
     * or empty span before the first event has been drained, and a zero-cell
     * viewport while a window is being dragged narrow. The grid stays in a
     * defined state with a legal shift, so a caller that ignores the return
     * value gets safe answers rather than undefined behaviour. */
    if (end <= base || cols_ == 0 || rows_ == 0) {
        cell_bytes_      = kMinCellBytes;
        log2_cell_bytes_ = static_cast<unsigned>(std::countr_zero(kMinCellBytes));
        return false;
    }

    const std::uint64_t span  = end - base;
    const auto cells = static_cast<std::uint64_t>(cols_) *
                       static_cast<std::uint64_t>(rows_);

    /* Ceiling division written as quotient-plus-remainder rather than
     * (span + cells - 1) / cells: the span can be most of the 64-bit range, and
     * the obvious form overflows exactly when the numbers get interesting. */
    std::uint64_t per_cell = span / cells;
    if (span % cells != 0) ++per_cell;

    cell_bytes_      = clamped_pow2(per_cell);
    log2_cell_bytes_ = static_cast<unsigned>(std::countr_zero(cell_bytes_));
    valid_           = true;
    return true;
}

bool Grid::set_bounds(std::uint64_t base, std::uint64_t end) noexcept {
    return configure(base, end, cols_, rows_);
}

bool Grid::set_viewport(int cols, int rows) noexcept {
    return configure(base_, end_, cols, rows);
}

std::size_t Grid::cell_count() const noexcept {
    if (cols_ <= 0 || rows_ <= 0) return 0;
    return static_cast<std::size_t>(cols_) * static_cast<std::size_t>(rows_);
}

bool Grid::index_of(std::uint64_t addr, std::size_t &out) const noexcept {
    if (!valid_ || addr < base_) return false;

    const std::uint64_t index = (addr - base_) >> log2_cell_bytes_;
    if (index >= cell_count()) return false; /* past what the grid covers */

    out = static_cast<std::size_t>(index);
    return true;
}

int Grid::row_of(std::size_t index) const noexcept {
    if (cols_ <= 0) return 0;
    return static_cast<int>(index / static_cast<std::size_t>(cols_));
}

int Grid::col_of(std::size_t index) const noexcept {
    if (cols_ <= 0) return 0;
    return static_cast<int>(index % static_cast<std::size_t>(cols_));
}

std::uint64_t Grid::offset_of_row(int row) const noexcept {
    if (!valid_ || row < 0) return 0;
    return static_cast<std::uint64_t>(row) *
           static_cast<std::uint64_t>(cols_) * cell_bytes_;
}

std::uint64_t Grid::addr_of_row(int row) const noexcept {
    return base_ + offset_of_row(row);
}

bool Grid::covers_whole_span() const noexcept {
    if (!valid_) return false;

    /* The question is `cell_bytes * cells >= span`, but that product overflows
     * at the top of the clamp range (1 GiB times a large viewport) and would
     * wrap to something small, reporting a shortfall that is not there. Ask the
     * equivalent question in the other direction instead -- are there at least
     * as many cells as the span needs -- which divides and cannot overflow. */
    const auto cells = static_cast<std::uint64_t>(cell_count());
    const std::uint64_t needed = span() / cell_bytes_ +
                                 (span() % cell_bytes_ != 0 ? 1u : 0u);
    return cells >= needed;
}

std::size_t format_byte_size(char *buf, std::size_t n,
                             std::uint64_t bytes) noexcept {
    if (buf == nullptr || n == 0) return 0;

    std::uint64_t whole = 0, tenths = 0;
    const std::size_t unit = scale_bytes(bytes, whole, tenths);

    int written;
    if (tenths != 0) {
        written = std::snprintf(buf, n, "%llu.%llu %s",
                                static_cast<unsigned long long>(whole),
                                static_cast<unsigned long long>(tenths),
                                kUnits[unit]);
    } else {
        written = std::snprintf(buf, n, "%llu %s",
                                static_cast<unsigned long long>(whole),
                                kUnits[unit]);
    }
    return written > 0 ? static_cast<std::size_t>(written) : 0;
}

std::size_t format_offset(char *buf, std::size_t n,
                          std::uint64_t offset) noexcept {
    if (buf == nullptr || n < kGutterWidth + 1) {
        if (buf != nullptr && n > 0) buf[0] = '\0';
        return 0;
    }

    std::uint64_t whole = 0, tenths = 0;
    const std::size_t unit = scale_bytes(offset, whole, tenths);

    char body[32];
    int len;
    if (tenths != 0) {
        len = std::snprintf(body, sizeof body, "%llu.%llu%s",
                            static_cast<unsigned long long>(whole),
                            static_cast<unsigned long long>(tenths),
                            kUnits[unit]);
    } else {
        len = std::snprintf(body, sizeof body, "%llu%s",
                            static_cast<unsigned long long>(whole),
                            kUnits[unit]);
    }
    if (len < 0) { buf[0] = '\0'; return 0; }

    /* Right-align into a fixed field. A label that changed width as the heap
     * grew would shift the whole map sideways mid-session. */
    auto body_len = static_cast<std::size_t>(len);
    if (body_len > kGutterWidth) body_len = kGutterWidth; /* truncate, never overflow */

    const std::size_t pad = kGutterWidth - body_len;
    std::memset(buf, ' ', pad);
    std::memcpy(buf + pad, body, body_len);
    buf[kGutterWidth] = '\0';
    return kGutterWidth;
}

} // namespace hv
