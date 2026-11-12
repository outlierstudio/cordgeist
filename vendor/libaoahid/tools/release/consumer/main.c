// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Smoke-links an installed package through its public C ABI and checks only
 * version identity, leaving protocol tests to the source-tree suite. */
#include <aoahid.h>

#include <stdint.h>

int main(void) {
    const uint32_t expected = ((uint32_t)AOAHID_VERSION_MAJOR << 24U) |
                              ((uint32_t)AOAHID_VERSION_MINOR << 16U) |
                              (uint32_t)AOAHID_VERSION_PATCH;
    return aoahid_version() == expected ? 0 : 1;
}
