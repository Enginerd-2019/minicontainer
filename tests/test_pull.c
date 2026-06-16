// Phase 8c: tests/test_pull.c
// Exercises cmd_pull()'s deterministic, hermetic paths — the argument
// validation and whitelist lookup — without touching the network or
// the filesystem bundle tree.  cmd_pull is the only public symbol in
// pull.c (the whitelist and helpers are file-local static), so these
// tests drive it through its CLI entry point.
//
// The actual download+extract path needs network + writes a bundle
// directory; it is verified by a live `minicontainer pull alpine` run
// on a connected host, NOT here (so `make test` stays hermetic, fast,
// and offline-safe — see the SKIP note below).  No root required.

#include "pull.h"
#include <stdio.h>
#include <string.h>

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL: %s — %s\n", __func__, msg);          \
            return 1;                                                   \
        }                                                               \
    } while (0)

/* No tag → usage message + non-zero return. */
static int test_pull_no_tag(void) {
    char *argv[] = { "pull", NULL };
    int rc = cmd_pull(1, argv);
    CHECK(rc != 0, "cmd_pull with no tag should return non-zero");
    printf("PASS: test_pull_no_tag\n");
    return 0;
}

/* Unknown tag → "Unknown tag" + non-zero return.  "debian" is
 * deliberately NOT in the whitelist (alpine, ubuntu only). */
static int test_pull_unknown_tag(void) {
    char *argv[] = { "pull", "debian", NULL };
    int rc = cmd_pull(2, argv);
    CHECK(rc != 0, "cmd_pull with unknown tag should return non-zero");
    printf("PASS: test_pull_unknown_tag\n");
    return 0;
}

/* Empty-string tag is also unknown (guards against a stray "" matching). */
static int test_pull_empty_tag(void) {
    char *argv[] = { "pull", "", NULL };
    int rc = cmd_pull(2, argv);
    CHECK(rc != 0, "cmd_pull with empty tag should return non-zero");
    printf("PASS: test_pull_empty_tag\n");
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_pull_no_tag();
    rc |= test_pull_unknown_tag();
    rc |= test_pull_empty_tag();
    /* The download+extract path (a known tag → curl/tar → bundle dir)
     * needs network and writes ./alpine-bundle; it is exercised by a
     * live `minicontainer pull alpine` run, not by the hermetic suite. */
    printf("SKIP: live download test (run `minicontainer pull alpine` on a connected host)\n");
    if (rc == 0) {
        printf("\nAll pull tests passed!\n");
    }
    return rc;
}
