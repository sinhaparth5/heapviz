/* heapviz - consumer-side attach helper, shared by the end-to-end tests.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Mirrors what the TUI will do in M2.3: poll for the target's segment, verify
 * the magic and ABI version, then map the whole ring.
 */

#ifndef HEAPVIZ_TEST_RING_ATTACH_H
#define HEAPVIZ_TEST_RING_ATTACH_H

#include "common/heapviz_abi.h"
#include "common/heapviz_ring.h"

#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace hvtest {

inline void sleep_ms(long ms) {
    timespec ts{ms / 1000, (ms % 1000) * 1000000};
    nanosleep(&ts, nullptr);
}

/* Returns the mapped ring, or nullptr on timeout. `bytes_out` receives the
 * mapping length for munmap. */
inline HvRingHeader *attach(std::int32_t pid, std::uint64_t &bytes_out,
                            int timeout_ms) {
    char name[HV_SHM_NAME_MAX];
    hv_shm_name(name, sizeof(name), pid);

    for (int waited = 0; waited < timeout_ms; waited += 5) {
        const int fd = shm_open(name, O_RDWR, 0);
        if (fd < 0) { sleep_ms(5); continue; }

        /* Map the header alone first: capacity is unknown until we read it. */
        void *probe = mmap(nullptr, sizeof(HvRingHeader),
                           PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (probe == MAP_FAILED) { close(fd); return nullptr; }

        auto *hdr = static_cast<HvRingHeader *>(probe);
        if (hdr->magic.load(std::memory_order_acquire) != HEAPVIZ_ABI_MAGIC) {
            /* Constructor still running; the magic publishes last. */
            munmap(probe, sizeof(HvRingHeader));
            close(fd);
            sleep_ms(5);
            continue;
        }
        if (hdr->abi_version != HEAPVIZ_ABI_VERSION) {
            std::fprintf(stderr, "ABI mismatch: target v%u, this build v%u\n",
                         hdr->abi_version, HEAPVIZ_ABI_VERSION);
            munmap(probe, sizeof(HvRingHeader));
            close(fd);
            return nullptr;
        }

        const std::uint64_t capacity = hdr->capacity;
        munmap(probe, sizeof(HvRingHeader));

        const auto page = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
        bytes_out = hv_mapping_size(capacity, page);

        void *full = mmap(nullptr, static_cast<std::size_t>(bytes_out),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (full == MAP_FAILED) return nullptr;

        auto *ring = static_cast<HvRingHeader *>(full);

        /* Claim the ring, which also releases a target started with
         * HEAPVIZ_WAIT_MS. Exclusive by design: `tail` is one cursor, so a
         * second consumer would advance it past events we never copied out. */
        std::uint32_t owner = 0;
        if (hv_ring_claim(ring, static_cast<std::uint32_t>(getpid()), &owner) == 0) {
            std::fprintf(stderr,
                         "target %d is already being watched by pid %u\n",
                         pid, owner);
            munmap(full, static_cast<std::size_t>(bytes_out));
            return nullptr;
        }
        return ring;
    }
    return nullptr;
}

} // namespace hvtest

#endif /* HEAPVIZ_TEST_RING_ATTACH_H */
