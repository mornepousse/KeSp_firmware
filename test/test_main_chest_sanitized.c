/* Minimal host runner for the SANITIZED chest test binary
 * (test_chest_sanitized, test/CMakeLists.txt). Built with
 * -fsanitize=address,undefined -fno-sanitize-recover=all: some of the chest
 * DMA decoder's bounds (main/comm/chest/chest_dma.c) guard a real
 * out-of-bounds stack write, and at -O0 an ordinary build can survive that
 * write "by accident" (the overflow clobbers unrelated locals and the
 * function can still return the expected value) — only a sanitizer reliably
 * turns the write itself red. This binary runs the chest test suites
 * (test_chest_proto.c, test_chest_dma.c, test_chest_oath.c, test_chest_view.c)
 * plus test_memlcd_model.c (review M-f, 2026-09-29: memlcd_couper_8/
 * memlcd_bas_coffre index caller-sized stack arrays the same way the DMA
 * decoder does, and every dependency it needs was already a source of this
 * binary) — never openpgp, which has two pre-existing ASan findings out of
 * scope for the chest link work — and is wired into test/CMakeLists.txt as
 * a POST_BUILD step on its own target, so `cmake --build test/build` (what
 * scripts/check.sh --fast runs) fails the build on a red run without
 * check.sh itself needing to know this binary exists. */
#include "test_framework.h"

int _test_pass_count = 0;
int _test_fail_count = 0;

extern void test_chest_proto(void);
extern void test_chest_dma(void);
extern void test_chest_oath(void);
extern void test_chest_view(void);
extern void test_memlcd_model(void);

int main(void)
{
    printf("KeSp chest sanitized tests (ASan+UBSan)\n");
    printf("========================================\n");

    test_chest_proto();
    test_chest_dma();
    test_chest_oath();
    test_chest_view();
    test_memlcd_model();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", _test_pass_count, _test_fail_count);
    printf("========================================\n");
    return _test_fail_count > 0 ? 1 : 0;
}
