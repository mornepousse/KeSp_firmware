/* Minimal test framework for host-side unit tests */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Shared counters — defined in test_main.c */
extern int _test_pass_count;
extern int _test_fail_count;

#define TEST_SUITE(name) \
    do { printf("\n--- %s ---\n", name); } while(0)

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        _test_fail_count++; \
    } else { \
        _test_pass_count++; \
    } \
} while(0)

#define TEST_ASSERT_EQ(a, b, msg) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        printf("  FAIL: %s: expected %lld, got %lld (%s:%d)\n", msg, _b, _a, __FILE__, __LINE__); \
        _test_fail_count++; \
    } else { \
        _test_pass_count++; \
    } \
} while(0)

#define TEST_RUN(fn) do { \
    printf("  [%s] ", #fn); \
    fn(); \
    printf("OK\n"); \
} while(0)

/* Matrix dimensions (match firmware) */
#define MATRIX_ROWS 5
#define MATRIX_COLS 13
#define NUM_KEYS (MATRIX_ROWS * MATRIX_COLS)
