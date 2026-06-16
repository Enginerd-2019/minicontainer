// Phase 7b: tests/test_cli.c
// Exercises parse_subcommand() — the only public function in cli.c that
// isn't gated on a subcommand handler.  cmd_* handlers themselves are
// covered indirectly by the Phase 0-7a regression suite (which invokes
// the runtime end-to-end through cmd_run via main.c's dispatcher).
//
// This test does NOT need root — parse_subcommand is pure string
// comparison.

#include "cli.h"
#include <assert.h>
#include <stdio.h>

#define CHECK(expr, expected)                                       \
    do {                                                            \
        subcommand_t got = (expr);                                  \
        if (got != (expected)) {                                    \
            fprintf(stderr, "FAIL: %s -> %d, expected %d\n",        \
                    #expr, (int)got, (int)(expected));              \
            return 1;                                               \
        }                                                           \
    } while (0)

static int test_known_subcommands(void) {
    CHECK(parse_subcommand("run"),     SUBCMD_RUN);
    CHECK(parse_subcommand("start"),   SUBCMD_START);
    CHECK(parse_subcommand("stop"),    SUBCMD_STOP);
    CHECK(parse_subcommand("exec"),    SUBCMD_EXEC);
    CHECK(parse_subcommand("inspect"), SUBCMD_INSPECT);
    CHECK(parse_subcommand("list"),    SUBCMD_LIST);
    CHECK(parse_subcommand("cleanup"), SUBCMD_CLEANUP);
    CHECK(parse_subcommand("pull"),    SUBCMD_PULL);     /* Phase 8c */
    printf("PASS: test_known_subcommands\n");
    return 0;
}

static int test_unknown_subcommands(void) {
    /* Mistyped, garbage, case-mismatched, and edge inputs. */
    CHECK(parse_subcommand(NULL),       SUBCMD_UNKNOWN);
    CHECK(parse_subcommand(""),         SUBCMD_UNKNOWN);
    CHECK(parse_subcommand("Run"),      SUBCMD_UNKNOWN);  /* case-sensitive */
    CHECK(parse_subcommand("RUN"),      SUBCMD_UNKNOWN);
    CHECK(parse_subcommand("runs"),     SUBCMD_UNKNOWN);  /* prefix-extra */
    CHECK(parse_subcommand("ru"),       SUBCMD_UNKNOWN);  /* prefix-only */
    CHECK(parse_subcommand("--pid"),    SUBCMD_UNKNOWN);  /* flag, not a subcommand */
    CHECK(parse_subcommand("/bin/sh"),  SUBCMD_UNKNOWN);  /* executable path */
    CHECK(parse_subcommand("not_a_subcommand_at_all"), SUBCMD_UNKNOWN);
    printf("PASS: test_unknown_subcommands\n");
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_known_subcommands();
    rc |= test_unknown_subcommands();
    if (rc == 0) {
        printf("\nAll CLI tests passed!\n");
    }
    return rc;
}