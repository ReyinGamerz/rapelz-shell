#include "rapelz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void rapelz_path(char *out, size_t size) {
    char cwd[4096];
    const char *logical = getenv("PWD");
    struct stat logical_stat, current_stat;

    int logical_valid = logical && logical[0] == '/' &&
        strlen(logical) < sizeof(cwd) &&
        stat(logical, &logical_stat) == 0 &&
        stat(".", &current_stat) == 0 &&
        logical_stat.st_dev == current_stat.st_dev &&
        logical_stat.st_ino == current_stat.st_ino;

    if (logical_valid) {
        snprintf(cwd, sizeof(cwd), "%s", logical);
    } else if (getcwd(cwd, sizeof(cwd)) == NULL) {
        snprintf(out, size, "?");
        return;
    }

    const char *home = getenv("HOME");
    if (home != NULL && strcmp(cwd, home) == 0) {
        snprintf(out, size, "~");
        return;
    }

    if (home != NULL) {
        size_t home_len = strlen(home);
        if (strncmp(cwd, home, home_len) == 0 &&
            (cwd[home_len] == '/' || cwd[home_len] == '\0')) {
            snprintf(out, size, "~%s", cwd + home_len);
            return;
        }
    }

    snprintf(out, size, "%s", cwd);
}

void rapelz_print_prompt(void) {
    char path[4096];
    rapelz_path(path, sizeof(path));

    printf("\033[1;35m╭─ \033[1;36m• \033[1;37m%s\n", path);
    printf("\033[1;35m╰─\033[1;36m❯ \033[0m");
    fflush(stdout);
}
