/* heapviz - terminal heap profiler, consumer side.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * M0 scaffold. The ANSI engine is M4 and attach/lifecycle is M2.3; for now this
 * binary exists to prove the C++20 half compiles the shared ABI header and
 * agrees with the C11 half about every field offset.
 */

#include "common/heapviz_abi.h"

#include <cstdio>
#include <cstring>
#include <string_view>
#include <unistd.h>

namespace {

constexpr std::string_view kVersion = "0.1.0-dev";

void print_version() {
    std::printf("heapviz %s (ABI v%u, %zu-byte events, %zu-byte ring header)\n",
                kVersion.data(), HEAPVIZ_ABI_VERSION,
                sizeof(HvEvent), sizeof(HvRingHeader));
}

void print_usage() {
    std::printf(
        "heapviz - live heap allocation visualiser\n"
        "\n"
        "usage:\n"
        "  heapviz -- <cmd> [args...]   launch a program under heapviz\n"
        "  heapviz --pid <pid>          attach to a running target\n"
        "  heapviz --version            print version and ABI details\n"
        "  heapviz --help               this message\n"
        "\n"
        "Not yet implemented: see ROADMAP.md milestone M2.3 for attach, M4 for\n"
        "the terminal engine. This build only verifies the shared ABI.\n");
}

} // namespace

int main(int argc, char **argv) {
    if (argc >= 2) {
        const std::string_view arg{argv[1]};
        if (arg == "--version" || arg == "-V") {
            print_version();
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        std::fprintf(stderr, "heapviz: not implemented yet: %s\n", argv[1]);
        std::fprintf(stderr, "heapviz: try --help\n");
        return 2;
    }
    print_usage();
    return 0;
}
