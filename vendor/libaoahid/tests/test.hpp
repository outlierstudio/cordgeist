// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Provides the minimal assertion surface shared by deterministic native tests;
 * it is test-only and never enters the library ABI. */
#pragma once

#include <cstdio>

extern int g_failures;

#define AOAHID_CHECK(expression)                                                                   \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #expression);    \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (false)

void test_item_writer();
void test_profiles();
void test_transport();
void test_identity();
