/* heapviz - target exercising indirect allocation paths.
 *
 * Copyright (C) 2026 Parth Sinha
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ROADMAP.md M1.4 says to confirm strdup/asprintf/getline are covered
 * transitively rather than assume it. They reach malloc through the PLT, so
 * interposing malloc should catch them, but "should" is not a test.
 *
 * Built as C++ so C++ operator new and std::string are covered too: those route
 * to malloc in libstdc++, and if that ever stopped being true, heapviz would
 * silently miss every allocation in a C++ program.
 *
 * Prints the number of allocating calls it made so the checker can compare.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr int kRounds = 2000;
}

int main() {
    long calls = 0;

    /* strdup: allocates internally, must show up as a malloc event. */
    for (int i = 0; i < kRounds; ++i) {
        char *s = strdup("heapviz transitive coverage check");
        if (s == nullptr) return 1;
        calls++;
        std::free(s);
    }

    /* asprintf: allocates the destination buffer. */
    for (int i = 0; i < kRounds; ++i) {
        char *s = nullptr;
        if (asprintf(&s, "iteration %d of %d", i, kRounds) < 0) return 1;
        calls++;
        std::free(s);
    }

    /* getline: allocates and grows the line buffer. */
    {
        std::FILE *f = std::tmpfile();
        if (f == nullptr) return 1;
        for (int i = 0; i < kRounds; ++i)
            std::fprintf(f, "line %d with enough text to force a real buffer\n", i);
        std::rewind(f);

        /* Reset the buffer every iteration. getline only allocates when the
         * caller hands it a null pointer; reusing the buffer (the normal way to
         * call it) allocates a handful of times for thousands of lines, which
         * would make the count here meaningless. */
        for (;;) {
            char *line = nullptr;
            std::size_t cap = 0;
            if (getline(&line, &cap, f) == -1) { std::free(line); break; }
            calls++;
            std::free(line);
        }
        std::fclose(f);
    }

    /* C++ operator new, both scalar and array. */
    for (int i = 0; i < kRounds; ++i) {
        auto *p = new int[64];
        p[0] = i;
        calls++;
        delete[] p;
    }

    /* std::string and std::vector past the small-buffer optimisation. */
    for (int i = 0; i < kRounds; ++i) {
        std::string s(256, 'x');
        std::vector<int> v(256, i);
        calls += 2;
        if (s.size() + v.size() == 0) return 1; /* defeat elision */
    }

    std::printf("transitive_target: %ld allocating calls\n", calls);
    return 0;
}
