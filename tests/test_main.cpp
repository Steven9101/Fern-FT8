// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "test.h"

namespace test {

namespace {
int failures_in_case = 0;
}

std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

void report(const char* file, int line, const std::string& what) {
    ++failures_in_case;
    std::fprintf(stderr, "    %s:%d: %s\n", file, line, what.c_str());
}

bool quick() {
    const char* q = std::getenv("FERN_TEST_QUICK");
    return q && std::strcmp(q, "1") == 0;
}

}  // namespace test

// Usage: fern-ft8-tests [name-substring]. FERN_TEST_QUICK=1 shortens the
// long statistical tests (used under the sanitizers, which run 10x slower).
int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int failed = 0;
    int run = 0;
    for (const test::Case& c : test::registry()) {
        if (filter && !std::strstr(c.name, filter))
            continue;
        ++run;
        test::failures_in_case = 0;
        const auto start = std::chrono::steady_clock::now();
        try {
            c.fn();
        } catch (const test::Stop&) {
        } catch (const std::exception& e) {
            test::report(__FILE__, __LINE__, std::string("exception: ") + e.what());
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (test::failures_in_case > 0) {
            ++failed;
            std::fprintf(stderr, "FAIL %s (%.0f ms)\n", c.name, ms);
        } else {
            std::fprintf(stderr, "ok   %s (%.0f ms)\n", c.name, ms);
        }
    }
    std::fprintf(stderr, "%d of %d tests passed\n", run - failed, run);
    return failed == 0 && run > 0 ? 0 : 1;
}
