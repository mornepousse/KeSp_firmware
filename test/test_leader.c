/* Tests for leader key sequence matching */
#include "test_framework.h"

#define LEADER_MAX_SEQ_LEN 4
#define LEADER_MAX_ENTRIES 16

typedef struct {
    uint8_t sequence[LEADER_MAX_SEQ_LEN];
    uint8_t result;
    uint8_t result_mod;
} leader_entry_t;

/* Simulate sequence matching logic */
static bool try_match(const leader_entry_t *entries, int n_entries,
                      const uint8_t *buffer, uint8_t buf_len,
                      uint8_t *out_result, uint8_t *out_mod)
{
    for (int i = 0; i < n_entries; i++) {
        if (entries[i].result == 0) continue;
        uint8_t entry_len = 0;
        for (int j = 0; j < LEADER_MAX_SEQ_LEN && entries[i].sequence[j] != 0; j++)
            entry_len++;
        if (entry_len != buf_len) continue;

        bool match = true;
        for (int j = 0; j < buf_len; j++) {
            if (entries[i].sequence[j] != buffer[j]) { match = false; break; }
        }
        if (match) {
            *out_result = entries[i].result;
            *out_mod = entries[i].result_mod;
            return true;
        }
    }
    return false;
}

static void test_leader_single_key_match(void) {
    leader_entry_t entries[1] = { { {0x04, 0, 0, 0}, 0x29, 0 } }; /* A → ESC */
    uint8_t buf[1] = {0x04};
    uint8_t result = 0, mod = 0;
    TEST_ASSERT(try_match(entries, 1, buf, 1, &result, &mod), "Single key match");
    TEST_ASSERT_EQ(result, 0x29, "Result = ESC");
}

static void test_leader_two_key_match(void) {
    leader_entry_t entries[1] = { { {0x09, 0x16, 0, 0}, 0x16, 0x01 } }; /* F+S → Ctrl+S */
    uint8_t buf[2] = {0x09, 0x16};
    uint8_t result = 0, mod = 0;
    TEST_ASSERT(try_match(entries, 1, buf, 2, &result, &mod), "Two key match");
    TEST_ASSERT_EQ(result, 0x16, "Result = S");
    TEST_ASSERT_EQ(mod, 0x01, "Mod = Ctrl");
}

static void test_leader_no_match(void) {
    leader_entry_t entries[1] = { { {0x04, 0, 0, 0}, 0x29, 0 } };
    uint8_t buf[1] = {0x05}; /* B, not A */
    uint8_t result = 0, mod = 0;
    TEST_ASSERT(!try_match(entries, 1, buf, 1, &result, &mod), "No match for wrong key");
}

static void test_leader_partial_no_match(void) {
    leader_entry_t entries[1] = { { {0x04, 0x05, 0, 0}, 0x29, 0 } }; /* A+B → ESC */
    uint8_t buf[1] = {0x04}; /* only A, not A+B */
    uint8_t result = 0, mod = 0;
    TEST_ASSERT(!try_match(entries, 1, buf, 1, &result, &mod), "Partial sequence doesn't match");
}

static void test_leader_empty_sequence(void) {
    leader_entry_t entries[1] = { { {0, 0, 0, 0}, 0x29, 0 } }; /* empty seq */
    uint8_t buf[1] = {0x04};
    uint8_t result = 0, mod = 0;
    TEST_ASSERT(!try_match(entries, 1, buf, 1, &result, &mod), "Empty entry skipped");
}

static void test_leader_unconfigured_entry(void) {
    leader_entry_t entries[1] = { { {0x04, 0, 0, 0}, 0, 0 } }; /* result=0 */
    uint8_t buf[1] = {0x04};
    uint8_t result = 0, mod = 0;
    TEST_ASSERT(!try_match(entries, 1, buf, 1, &result, &mod), "result=0 entry skipped");
}

static void test_leader_multiple_entries(void) {
    leader_entry_t entries[3] = {
        { {0x04, 0, 0, 0}, 0x29, 0 },    /* A → ESC */
        { {0x05, 0, 0, 0}, 0x28, 0 },    /* B → Enter */
        { {0x04, 0x05, 0, 0}, 0x2A, 0 }, /* A+B → Backspace */
    };
    uint8_t buf1[1] = {0x05};
    uint8_t result = 0, mod = 0;
    TEST_ASSERT(try_match(entries, 3, buf1, 1, &result, &mod), "B matches entry 1");
    TEST_ASSERT_EQ(result, 0x28, "B → Enter");

    uint8_t buf2[2] = {0x04, 0x05};
    TEST_ASSERT(try_match(entries, 3, buf2, 2, &result, &mod), "A+B matches entry 2");
    TEST_ASSERT_EQ(result, 0x2A, "A+B → Backspace");
}

static void test_leader_four_key_sequence(void) {
    leader_entry_t entries[1] = { { {0x04, 0x05, 0x06, 0x07}, 0x29, 0x02 } };
    uint8_t buf[4] = {0x04, 0x05, 0x06, 0x07};
    uint8_t result = 0, mod = 0;
    TEST_ASSERT(try_match(entries, 1, buf, 4, &result, &mod), "4-key match");
    TEST_ASSERT_EQ(mod, 0x02, "Mod = Shift");
}

void test_leader(void) {
    TEST_SUITE("Leader Key");
    TEST_RUN(test_leader_single_key_match);
    TEST_RUN(test_leader_two_key_match);
    TEST_RUN(test_leader_no_match);
    TEST_RUN(test_leader_partial_no_match);
    TEST_RUN(test_leader_empty_sequence);
    TEST_RUN(test_leader_unconfigured_entry);
    TEST_RUN(test_leader_multiple_entries);
    TEST_RUN(test_leader_four_key_sequence);
}
