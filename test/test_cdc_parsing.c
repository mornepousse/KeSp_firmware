/* Test CDC command parsing logic */
#include "test_framework.h"
#include <ctype.h>

/* Reproduce the FIFO and line assembly logic from cdc_acm_com.c */

#define CDC_CMD_MAX_LEN 1024
#define CDC_CMD_FIFO_DEPTH 4

typedef struct {
    char line[CDC_CMD_MAX_LEN];
    uint16_t len;
} cdc_cmd_t;

static cdc_cmd_t fifo[CDC_CMD_FIFO_DEPTH];
static int fifo_head = 0, fifo_tail = 0, fifo_count = 0;

static void fifo_init(void) {
    fifo_head = fifo_tail = fifo_count = 0;
}

static bool fifo_push(const char *line, uint16_t len) {
    if (fifo_count >= CDC_CMD_FIFO_DEPTH) return false;
    if (len >= CDC_CMD_MAX_LEN) len = CDC_CMD_MAX_LEN - 1;
    memcpy(fifo[fifo_head].line, line, len);
    fifo[fifo_head].line[len] = '\0';
    fifo[fifo_head].len = len;
    fifo_head = (fifo_head + 1) % CDC_CMD_FIFO_DEPTH;
    fifo_count++;
    return true;
}

static bool fifo_pop(cdc_cmd_t *out) {
    if (fifo_count == 0) return false;
    *out = fifo[fifo_tail];
    fifo_tail = (fifo_tail + 1) % CDC_CMD_FIFO_DEPTH;
    fifo_count--;
    return true;
}

/* Reproduce line assembly from receive_data() */
static char line_buf[CDC_CMD_MAX_LEN];
static uint16_t line_pos = 0;
static bool last_was_cr = false;
static bool overflowed = false;

static void line_reset(void) {
    line_pos = 0;
    last_was_cr = false;
    overflowed = false;
}

static void flush_line(void) {
    if (line_pos > 0 && !overflowed) {
        fifo_push(line_buf, line_pos);
    }
    line_pos = 0;
    overflowed = false;
}

static void receive_chunk(const char *data, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        char c = data[i];

        if (c == '\n') {
            if (last_was_cr) {
                last_was_cr = false;
                continue;
            }
            flush_line();
            last_was_cr = false;
            continue;
        }
        if (c == '\r') {
            flush_line();
            last_was_cr = true;
            continue;
        }
        last_was_cr = false;

        if (overflowed) continue;

        if (line_pos < CDC_CMD_MAX_LEN - 1) {
            line_buf[line_pos++] = c;
        } else {
            overflowed = true;
        }
    }
}

/* Test: simple command terminated by \r\n */
void test_parse_simple_crlf(void) {
    fifo_init(); line_reset();
    receive_chunk("PING\r\n", 6);
    cdc_cmd_t cmd;
    TEST_ASSERT(fifo_pop(&cmd), "command popped");
    TEST_ASSERT(strcmp(cmd.line, "PING") == 0, "command is PING");
    TEST_ASSERT_EQ(cmd.len, 4, "len = 4");
}

/* Test: command terminated by \n only */
void test_parse_lf_only(void) {
    fifo_init(); line_reset();
    receive_chunk("KEYSTATS\n", 9);
    cdc_cmd_t cmd;
    TEST_ASSERT(fifo_pop(&cmd), "command popped");
    TEST_ASSERT(strcmp(cmd.line, "KEYSTATS") == 0, "command is KEYSTATS");
}

/* Test: command terminated by \r only */
void test_parse_cr_only(void) {
    fifo_init(); line_reset();
    receive_chunk("VERSION?\r", 9);
    cdc_cmd_t cmd;
    TEST_ASSERT(fifo_pop(&cmd), "command popped");
    TEST_ASSERT(strcmp(cmd.line, "VERSION?") == 0, "command is VERSION?");
}

/* Test: multiple commands in one chunk */
void test_parse_multiple_commands(void) {
    fifo_init(); line_reset();
    receive_chunk("PING\r\nVERSION?\r\nKEYSTATS\r\n", 26);

    cdc_cmd_t cmd;
    TEST_ASSERT(fifo_pop(&cmd), "first cmd");
    TEST_ASSERT(strcmp(cmd.line, "PING") == 0, "first = PING");

    TEST_ASSERT(fifo_pop(&cmd), "second cmd");
    TEST_ASSERT(strcmp(cmd.line, "VERSION?") == 0, "second = VERSION?");

    TEST_ASSERT(fifo_pop(&cmd), "third cmd");
    TEST_ASSERT(strcmp(cmd.line, "KEYSTATS") == 0, "third = KEYSTATS");

    TEST_ASSERT(!fifo_pop(&cmd), "no more commands");
}

/* Test: split across chunks */
void test_parse_split_chunks(void) {
    fifo_init(); line_reset();
    receive_chunk("KEY", 3);
    receive_chunk("STATS\r\n", 7);
    cdc_cmd_t cmd;
    TEST_ASSERT(fifo_pop(&cmd), "command assembled");
    TEST_ASSERT(strcmp(cmd.line, "KEYSTATS") == 0, "KEYSTATS from 2 chunks");
}

/* Test: empty lines are ignored */
void test_parse_empty_lines(void) {
    fifo_init(); line_reset();
    receive_chunk("\r\n\r\nPING\r\n", 10);
    cdc_cmd_t cmd;
    /* Empty lines produce 0-length — flush_line skips them */
    TEST_ASSERT(fifo_pop(&cmd), "PING popped");
    TEST_ASSERT(strcmp(cmd.line, "PING") == 0, "command is PING");
}

/* Test: FIFO overflow (more than FIFO_DEPTH commands) */
void test_parse_fifo_overflow(void) {
    fifo_init(); line_reset();
    /* Push FIFO_DEPTH+1 commands */
    for (int i = 0; i < CDC_CMD_FIFO_DEPTH + 1; i++) {
        char buf[16];
        int n = snprintf(buf, sizeof(buf), "CMD%d\r\n", i);
        receive_chunk(buf, n);
    }
    /* Should have exactly FIFO_DEPTH */
    TEST_ASSERT_EQ(fifo_count, CDC_CMD_FIFO_DEPTH, "FIFO capped at depth");

    cdc_cmd_t cmd;
    TEST_ASSERT(fifo_pop(&cmd), "can pop first");
    TEST_ASSERT(strcmp(cmd.line, "CMD0") == 0, "first is CMD0");
}

/* Test: line overflow (command > CDC_CMD_MAX_LEN) is dropped */
void test_parse_line_overflow(void) {
    fifo_init(); line_reset();

    /* Send a line longer than CDC_CMD_MAX_LEN */
    char big[CDC_CMD_MAX_LEN + 100];
    memset(big, 'A', sizeof(big));
    receive_chunk(big, sizeof(big));
    receive_chunk("\r\n", 2);

    /* Overflowed line should be dropped */
    cdc_cmd_t cmd;
    TEST_ASSERT(!fifo_pop(&cmd), "overflowed line dropped");

    /* Next normal command should work */
    receive_chunk("OK\r\n", 4);
    TEST_ASSERT(fifo_pop(&cmd), "normal cmd after overflow");
    TEST_ASSERT(strcmp(cmd.line, "OK") == 0, "command is OK");
}

/* Test: command with arguments */
void test_parse_command_with_args(void) {
    fifo_init(); line_reset();
    receive_chunk("SETLAYER0:0x04,0x05,0x06\r\n", 26);
    cdc_cmd_t cmd;
    TEST_ASSERT(fifo_pop(&cmd), "command popped");
    TEST_ASSERT(strncmp(cmd.line, "SETLAYER0:", 10) == 0, "starts with SETLAYER0:");
}

/* Test: strncasecmp matching (reproduces parse_and_execute pattern) */
void test_command_matching(void) {
    const char *line = "LAYOUT?";
    TEST_ASSERT(strncasecmp(line, "LAYOUT?", 7) == 0, "LAYOUT? matches");
    TEST_ASSERT(line[7] == '\0', "no suffix");

    /* "LAYOUTS?" does NOT match "LAYOUT?" with strncasecmp(7) because
     * position 6 is 'S' vs '?' — the firmware checks this explicitly */
    const char *line2 = "LAYOUTS?";
    TEST_ASSERT(strncasecmp(line2, "LAYOUT?", 7) != 0, "LAYOUTS? does not match LAYOUT?");
    TEST_ASSERT(strncasecmp(line2, "LAYOUTS?", 8) == 0, "LAYOUTS? matches with len 8");

    const char *line3 = "layout?";
    TEST_ASSERT(strncasecmp(line3, "LAYOUT?", 7) == 0, "case insensitive match");
}

void test_cdc_parsing(void) {
    TEST_SUITE("CDC Command Parsing");
    TEST_RUN(test_parse_simple_crlf);
    TEST_RUN(test_parse_lf_only);
    TEST_RUN(test_parse_cr_only);
    TEST_RUN(test_parse_multiple_commands);
    TEST_RUN(test_parse_split_chunks);
    TEST_RUN(test_parse_empty_lines);
    TEST_RUN(test_parse_fifo_overflow);
    TEST_RUN(test_parse_line_overflow);
    TEST_RUN(test_parse_command_with_args);
    TEST_RUN(test_command_matching);
}
