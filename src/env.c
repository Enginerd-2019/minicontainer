// Note: _GNU_SOURCE is provided by the Makefile via -D_GNU_SOURCE.
#include "env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PATH "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

char **build_container_env(char *const *custom_env, bool enable_debug) {
    /* Minimal container baseline. Never leak host PATH/HOME/SHELL —
     * they reference paths that don't exist inside the container. */
    char *defaults[] = {
        "PATH=" DEFAULT_PATH,
        "HOME=/root",
        "TERM=xterm",
        NULL
    };

    int count = 0;
    while (defaults[count]) count++;

    /* calloc gives us NULL-terminated-everywhere for free. */
    int max_entries = count + MAX_ENV_ENTRIES + 1;
    char **env = calloc(max_entries, sizeof(char *));
    if (!env) {
        perror("calloc");
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        env[i] = defaults[i];
    }

    if (custom_env) {
        for (int i = 0; custom_env[i]; i++) {
            const char *eq = strchr(custom_env[i], '=');
            if (!eq) continue;  // skip malformed (no '=')

            size_t key_len = eq - custom_env[i];
            bool replaced = false;

            for (int j = 0; j < count; j++) {
                if (strncmp(env[j], custom_env[i], key_len + 1) == 0) {
                    env[j] = custom_env[i];
                    replaced = true;
                    break;
                }
            }

            /* Defense-in-depth bounds check — see Phase 5 §3.7. */
            if (!replaced && count < max_entries - 1) {
                env[count++] = custom_env[i];
            }
        }
    }

    env[count] = NULL;

    if (enable_debug) {
        printf("[parent] Container environment:\n");
        for (int i = 0; env[i]; i++) {
            printf("[parent]   %s\n", env[i]);
        }
    }

    return env;
}