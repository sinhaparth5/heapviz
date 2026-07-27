/* heapviz - shared memory round-trip, consumer half (C++20).
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Maps the segment shm_probe_writer created (in C11) and asserts every header
 * field and every event byte. Unlinks on the way out, including on failure, so
 * a failed run does not leave a stale segment behind for the next one.
 */

#include "common/heapviz_abi.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr std::uint64_t kProbeCapacity = 1024;
constexpr std::uint32_t kProbeEvents   = 64;

int g_failures = 0;

template <typename T>
void expect_eq(const char *what, T got, T want) {
    if (got != want) {
        std::fprintf(stderr, "  FAIL %s: got %llu, want %llu\n", what,
                     static_cast<unsigned long long>(got),
                     static_cast<unsigned long long>(want));
        ++g_failures;
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: shm_probe_reader <writer-pid>\n");
        return 2;
    }
    const auto pid = static_cast<std::int32_t>(std::strtol(argv[1], nullptr, 10));

    char name[HV_SHM_NAME_MAX];
    hv_shm_name(name, sizeof(name), pid);

    const int fd = shm_open(name, O_RDONLY, 0);
    if (fd < 0) {
        std::perror("reader: shm_open");
        return 1;
    }

    const auto page  = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
    const auto bytes = hv_mapping_size(kProbeCapacity, page);

    void *map = mmap(nullptr, static_cast<std::size_t>(bytes), PROT_READ,
                     MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        std::perror("reader: mmap");
        shm_unlink(name);
        return 1;
    }

    // Cast away const-ness of the mapping only to read atomics; we opened
    // PROT_READ, so any accidental store would fault rather than corrupt.
    auto *h = static_cast<HvRingHeader *>(map);

    // Acquire pairs with the writer's release store on magic: if we see the
    // magic, every field written before it is visible.
    const std::uint64_t magic = h->magic.load(std::memory_order_acquire);
    expect_eq<std::uint64_t>("magic", magic, HEAPVIZ_ABI_MAGIC);

    expect_eq<std::uint32_t>("abi_version", h->abi_version, HEAPVIZ_ABI_VERSION);
    expect_eq<std::uint32_t>("event_size", h->event_size,
                             static_cast<std::uint32_t>(sizeof(HvEvent)));
    expect_eq<std::uint64_t>("capacity", h->capacity, kProbeCapacity);
    expect_eq<std::int32_t>("pid", h->pid, pid);
    expect_eq<std::uint32_t>("flags", h->flags, 0u);

    if (std::strcmp(h->comm, "shm_probe") != 0) {
        std::fprintf(stderr, "  FAIL comm: got \"%s\", want \"shm_probe\"\n",
                     h->comm);
        ++g_failures;
    }
    if (h->start_time_ns == 0) {
        std::fprintf(stderr, "  FAIL start_time_ns: unset\n");
        ++g_failures;
    }

    expect_eq<std::uint64_t>("head", h->head.load(std::memory_order_relaxed),
                             kProbeEvents);
    expect_eq<std::uint64_t>("tail", h->tail.load(std::memory_order_relaxed), 0u);
    expect_eq<std::uint64_t>("dropped",
                             h->dropped.load(std::memory_order_relaxed), 7u);
    expect_eq<std::uint64_t>("total_events",
                             h->total_events.load(std::memory_order_relaxed),
                             kProbeEvents + 7u);
    expect_eq<std::uint32_t>("producer_exited",
                             h->producer_exited.load(std::memory_order_relaxed),
                             0u);

    // Every event, byte for byte. This is what actually proves the C11 writer
    // and the C++20 reader agree about HvEvent's layout in shared memory.
    for (std::uint32_t i = 0; i < kProbeEvents; ++i) {
        const HvEvent *e = hv_ring_slot(h, i);
        char label[64];

        std::snprintf(label, sizeof(label), "event[%u].timestamp", i);
        expect_eq<std::uint64_t>(label, e->timestamp, UINT64_C(1000000) + i);

        std::snprintf(label, sizeof(label), "event[%u].ptr", i);
        expect_eq<std::uint64_t>(label, e->ptr,
                                 UINT64_C(0x0000dead00000000) + std::uint64_t{i} * 16u);

        std::snprintf(label, sizeof(label), "event[%u].size", i);
        expect_eq<std::uint64_t>(label, e->size, std::uint64_t{i + 1} * 64u);

        std::snprintf(label, sizeof(label), "event[%u].usable", i);
        expect_eq<std::uint32_t>(label, e->usable, (i + 1u) * 64u + 8u);

        std::snprintf(label, sizeof(label), "event[%u].op", i);
        expect_eq<std::uint32_t>(label, std::uint32_t{e->op},
                                 i % static_cast<std::uint32_t>(HV_OP__COUNT));

        std::snprintf(label, sizeof(label), "event[%u].tid", i);
        expect_eq<std::uint32_t>(label, hv_event_get_tid(e), 0x100u + i);
    }

    munmap(map, static_cast<std::size_t>(bytes));
    shm_unlink(name);

    if (g_failures != 0) {
        std::fprintf(stderr, "shm round-trip: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("shm round-trip: header + %u events verified across C11/C++20\n",
                kProbeEvents);
    return 0;
}
