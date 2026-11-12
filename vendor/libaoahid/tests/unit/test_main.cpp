// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Runs the allocation-free HID/profile unit suite; integration executables
 * own transport and threading coverage. */
#include "test.hpp"

#include <cstring>

int g_failures = 0;

namespace {
struct Suite {
    const char* name;
    void (*run)();
};

constexpr Suite kSuites[] = {{"item_writer", test_item_writer},
                             {"profiles", test_profiles},
                             {"transport", test_transport},
                             {"identity", test_identity}};
} // namespace

// With no arguments every suite runs; otherwise only the named ones.
int main(const int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        bool known = false;
        for (const Suite& suite : kSuites) {
            known = known || std::strcmp(argv[index], suite.name) == 0;
        }
        if (!known) {
            std::fprintf(stderr, "unknown suite: %s\n", argv[index]);
            return 2;
        }
    }
    for (const Suite& suite : kSuites) {
        bool selected = argc < 2;
        for (int index = 1; index < argc; ++index) {
            selected = selected || std::strcmp(argv[index], suite.name) == 0;
        }
        if (selected) {
            suite.run();
        }
    }
    return g_failures == 0 ? 0 : 1;
}
