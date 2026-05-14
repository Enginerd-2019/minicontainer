#ifndef ENV_H
#define ENV_H

#include <stdbool.h>

/**
 * Maximum number of entries (baseline + custom) the returned env array
 * can hold. Exposed so callers (main.c, test_*.c) can size their input
 * arrays consistently.
 */
#define MAX_ENV_ENTRIES 256

/**
 * Build a clean container environment.
 *
 * Replaces host environ with a minimal baseline (PATH, HOME, TERM) and
 * merges custom_env entries: if a key already exists in the baseline,
 * replace; otherwise append.
 *
 * Phase 3 §3.4 introduced this. Phase 5 §3.7 refactored it to be safe
 * to call from any context (calloc + bounds check). Phase 7a moves it
 * into its own utility module so subcommands (Phase 7b) and bundle
 * loaders (Phase 8) can reuse it.
 *
 * Ownership: caller owns the returned pointer and must free() it. The
 * function does not duplicate the string contents — they point into
 * defaults[] (static storage) or custom_env[] (caller's storage).
 *
 * @param custom_env    NULL-terminated array of "KEY=VALUE" strings, or NULL.
 * @param enable_debug  Print the final environment for inspection.
 * @return  Heap-allocated, NULL-terminated env array; NULL on allocation failure.
 */
char **build_container_env(char *const *custom_env, bool enable_debug);

#endif // ENV_H