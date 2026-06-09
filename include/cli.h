#ifndef CLI_H
#define CLI_H

/**
 * Subcommand identity returned by parse_subcommand().  SUBCMD_UNKNOWN
 * means the first non-option argument did NOT match any known
 * subcommand — the dispatcher decides what to do next (typically
 * implicit-run for backwards compat with Phase 7a invocations).
 */
typedef enum {
    SUBCMD_UNKNOWN,
    SUBCMD_RUN,
    SUBCMD_START,
    SUBCMD_STOP,
    SUBCMD_EXEC,
    SUBCMD_INSPECT,
    SUBCMD_LIST,
    SUBCMD_CLEANUP,
    SUBCMD_STATS,       /* NEW in 8a */
    SUBCMD_TOP,         /* NEW in 8a */
    SUBCMD_NETSTAT,     /* NEW in 8a */
} subcommand_t;

/**
 * Trivial strcmp dispatch on the literal subcommand name.
 * @param arg  The subcommand string (typically argv[1]).
 * @return     The matching subcommand_t, or SUBCMD_UNKNOWN.
 */
subcommand_t parse_subcommand(const char *arg);

/**
 * Per-subcommand entry points.  Each takes the argv slice STARTING at
 * the subcommand token itself (argv[0] = "run" / "stop" / ...), so
 * getopt_long(3) inside the handler sees its own argv[0] for error
 * messages.  Returns the process exit code.
 */
int cmd_run(int argc, char *argv[]);
int cmd_start(int argc, char *argv[]);
int cmd_stop(int argc, char *argv[]);
int cmd_exec(int argc, char *argv[]);
int cmd_inspect(int argc, char *argv[]);
int cmd_list(int argc, char *argv[]);
int cmd_cleanup(int argc, char *argv[]);

/* New in 8a: introspection subcommand handlers. Each wraps a
 * show_container_* primitive from inspector.c. */
int cmd_stats(int argc, char *argv[]);
int cmd_top(int argc, char *argv[]);
int cmd_netstat(int argc, char *argv[]);

/**
 * Print top-level usage (subcommand list + flag summary) to stderr.
 * @param progname  argv[0] from main().
 */
void cli_usage(const char *progname);

#endif // CLI_H