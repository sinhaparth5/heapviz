/* heapviz - stale shared-memory segment cleanup.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The interceptor unlinks its segment from a destructor, which SIGKILL skips.
 * A killed target therefore leaves its ring behind in /dev/shm, sized by
 * HEAPVIZ_CAPACITY, until something removes it. A few kills during development
 * add up to real tmpfs.
 *
 * This lives on the TUI side rather than in the interceptor because scanning a
 * directory means opendir/readdir, which allocate. Ground rule #1 forbids that
 * inside the hook, and a reaper that ran in every target's constructor would
 * also be doing the work N times for no reason.
 */

#ifndef HEAPVIZ_TUI_SHM_CLEANUP_H
#define HEAPVIZ_TUI_SHM_CLEANUP_H

#include <cstdint>
#include <string>
#include <vector>

namespace hv {

struct StaleSegment {
    std::string   name;      /* as it appears under /dev/shm            */
    std::int32_t  pid = 0;   /* the producer it was created for         */
    std::uint64_t bytes = 0; /* on-disk size, for the report            */
    bool          owner_alive = false;
};

/* Lists every heapviz segment, marking which ones belong to a live process.
 * Segments belonging to another user are not visible: the interceptor creates
 * them 0600, so this only ever sees our own. */
std::vector<StaleSegment> list_segments();

/* Unlinks segments whose producer is gone. Returns the number removed and adds
 * the bytes reclaimed to `bytes_freed` when non-null.
 *
 * A live producer's segment is never touched, so this is safe to run while
 * other targets are being profiled. */
int reap_stale_segments(bool dry_run, std::uint64_t *bytes_freed = nullptr);

} // namespace hv

#endif /* HEAPVIZ_TUI_SHM_CLEANUP_H */
