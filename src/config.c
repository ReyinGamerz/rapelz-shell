#define _POSIX_C_SOURCE 200809L
#include "rapelz.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void config_path(char *path, size_t size) {
    const char *override = getenv("RAPELZ_CONFIG");
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");

    if (override && *override) {
        snprintf(path, size, "%s", override);
    } else if (xdg && *xdg) {
        snprintf(path, size, "%s/rapelz/config", xdg);
    } else if (home && *home) {
        snprintf(path, size, "%s/.config/rapelz/config", home);
    } else {
        path[0] = '\0';
    }
}

static int read_config_line(FILE *fp, char *line, size_t size) {
    if (!fgets(line, (int)size, fp)) return 0;

    size_t used = strlen(line);
    if (used > 0 && line[used - 1] != '\n' && !feof(fp)) {
        int ch = fgetc(fp);
        if (ch == '\r') {
            int next = fgetc(fp);
            if (next != '\n' && next != EOF) ch = next;
            else ch = '\n';
        }
        if (ch != '\n' && ch != EOF) {
            while (ch != '\n' && ch != EOF) ch = fgetc(fp);
            return -1;
        }
    }

    line[strcspn(line, "\r\n")] = '\0';
    return 1;
}

void rapelz_config_load(void) {
    char path[4096];
    config_path(path, sizeof(path));
    if (!path[0]) return;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (errno != ENOENT) {
            fprintf(stderr, "rapelz: gagal membaca config: %s\n", strerror(errno));
        }
        return;
    }

    char line[RAPELZ_INPUT_SIZE];
    int line_result;
    while ((line_result = read_config_line(fp, line, sizeof(line))) != 0) {
        if (line_result < 0) {
            fprintf(stderr, "rapelz: baris config terlalu panjang; diabaikan\n");
            continue;
        }
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;

        if (rapelz_run_line(p) != 0)
            fprintf(stderr, "rapelz: config: command gagal: %s\n", p);
    }

    fclose(fp);
}
