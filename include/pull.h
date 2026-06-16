#ifndef PULL_H
#define PULL_H

#include <stdbool.h>

/*
 * Run the `pull` subcommand. Looks up the tag in a hardcoded
 * whitelist (alpine, ubuntu), downloads the corresponding rootfs
 * tarball, extracts it to a bundle directory, writes a default
 * config.json.
 *
 * argc, argv   From main; `argv[0]` = "pull", `argv[1]` = <tag>.
 *
 * Returns 0 on success, non-zero on failure (unknown tag, download
 * failed, extraction failed). Error messages go to stderr.
 *
 * Concurrency: not thread-safe (writes to fixed bundle_dir). Don't
 * run two pulls of the same tag simultaneously.
 */
int cmd_pull(int argc, char *argv[]);

#endif /* PULL_H */
