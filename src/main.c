// Note: _GNU_SOURCE is provided by the Makefile via -D_GNU_SOURCE.
//
// Phase 7b: thin subcommand dispatcher.  Every prior body (flag
// parsing, config construction, container_exec, result handling) lives
// in cli.c::cmd_run now.  This file's job is exclusively:
//
//   1. Look at argv[1] and decide which cmd_* handler to call.
//   2. Fall back to implicit-run when argv[1] is a flag or an
//      executable path (Phase 7a backwards-compat: `./minicontainer
//      --pid /bin/echo hi` keeps working without a `run` keyword).
//   3. Print top-level usage and exit 1 on anything else.
//
// Anything more complex than this belongs in cli.c.

#include "cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        cli_usage(argv[0]);
        return 1;
    }

    /* Explicit subcommand. */
    subcommand_t sub = parse_subcommand(argv[1]);
    switch (sub) {
        case SUBCMD_RUN:     return cmd_run    (argc - 1, argv + 1);
        case SUBCMD_START:   return cmd_start  (argc - 1, argv + 1);
        case SUBCMD_STOP:    return cmd_stop   (argc - 1, argv + 1);
        case SUBCMD_EXEC:    return cmd_exec   (argc - 1, argv + 1);
        case SUBCMD_INSPECT: return cmd_inspect(argc - 1, argv + 1);
        case SUBCMD_LIST:    return cmd_list   (argc - 1, argv + 1);
        case SUBCMD_CLEANUP: return cmd_cleanup(argc - 1, argv + 1);
        case SUBCMD_UNKNOWN:
            break;  /* fall through to implicit-run logic below */
    }

    /* Phase 7a backwards-compat: if argv[1] starts with '-' it's a
     * flag (so the user wrote `minicontainer --pid /bin/sh` with no
     * subcommand); if it's an executable path it's the target program
     * (so the user wrote `minicontainer /bin/sh` with no flags either).
     * Either way, route to cmd_run with the *original* argv so getopt
     * sees the full picture.  cmd_run resets optind to 1 internally. */
    if (argv[1][0] == '-' || access(argv[1], X_OK) == 0) {
        return cmd_run(argc, argv);
    }

    fprintf(stderr, "minicontainer: unknown subcommand '%s'\n", argv[1]);
    cli_usage(argv[0]);
    return 1;
}