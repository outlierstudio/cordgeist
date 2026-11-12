// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Compile-only regression for the Windows macros pulled in by real libusb. */

#include "transport/libusb_include.hpp"

#include <algorithm>
#include <limits>

#if !defined(NOMINMAX)
#error "libusb_include.hpp must define NOMINMAX before the libusb header"
#endif

int main() {
    const int smaller = std::min(2, 3);
    const int maximum = std::numeric_limits<int>::max();
    return smaller == 2 && maximum > 0 ? 0 : 1;
}
