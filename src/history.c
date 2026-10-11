#define _POSIX_C_SOURCE 200809L
#include "rapelz.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char history[RAPELZ_HISTORY_MAX][RAPELZ_INPUT_SIZE];
static size_t history_count;
static size_t history_index;
static char history_path[4096];
static int history_loaded;
static FILE *history_file;
static size_t search_index;
static char search_query[RAPELZ_INPUT_SIZE];

static int choose_history_path(void) {
    const char *override = getenv("RAPELZ_HISTORY_FILE");
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    int n;

    if (override && *override)
        n = snprintf(history_path, sizeof(history_path), "%s", override);
    else if (xdg && *xdg)
        n = snprintf(history_path, sizeof(history_path), "%s/rapelz/history", xdg);
    else if (home && *home)
        n = snprintf(history_path, sizeof(history_path), "%s/.rapelz_history", home);
    else {
        history_path[0] = '\0';
        return 0;
    }

    if (n < 0 || (size_t)n >= sizeof(history_path)) {
        history_path[0] = '\0';
        fprintf(stderr, "rapelz: jalur history terlalu panjang; history dinonaktifkan\n");
        return -1;
    }
    if (history_path[0] != '/') {
        fprintf(stderr, "rapelz: jalur history harus absolut; history dinonaktifkan\n");
        history_path[0] = '\0';
        return -1;
    }
    return 0;
}

static int ensure_parent_dir(void) {
    char temp[4096];
    if (!history_path[0]) return -1;
    snprintf(temp, sizeof(temp), "%s", history_path);
    char *slash = strrchr(temp, '/');
    if (!slash || slash == temp) return 0;
    *slash = '\0';

    char current[4096] = {0};
    const char *p = temp;
    if (*p == '/') {
        strcpy(current, "/");
        p++;
    }

    while (*p) {
        const char *next = strchr(p, '/');
        size_t len = next ? (size_t)(next - p) : strlen(p);
        if (len) {
            if (strlen(current) + len + 2 >= sizeof(current)) return -1;
            if (strcmp(current, "/") != 0 && current[0]) strcat(current, "/");
            strncat(current, p, len);
            if (mkdir(current, 0700) != 0 && errno != EEXIST) return -1;
        }
        if (!next) break;
        p = next + 1;
    }
    return 0;
}

static void open_history_file(void) {
    if (!history_path[0]) return;
    if (ensure_parent_dir() != 0) return;

    int flags = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(history_path, flags, 0600);
    if (fd < 0) return;

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        fchmod(fd, 0600) != 0) {
        close(fd);
        return;
    }

    history_file = fdopen(fd, "a");
    if (!history_file) {
        close(fd);
        return;
    }
    setvbuf(history_file, NULL, _IOLBF, 0);
}

/*
 * Baca satu rekaman history utuh. fgets() biasa memecah rekaman panjang menjadi
 * beberapa command palsu; bytes kontrol juga tidak boleh diteruskan ke editor
 * sebagai autosuggestion atau ditampilkan sebagai escape sequence terminal.
 * -1 berarti rekaman ditolak, 0 EOF, dan 1 rekaman valid (mungkin kosong).
 */
static int read_history_line(FILE *fp, char *line, size_t cap) {
    size_t len = 0;
    int ch;
    int saw_data = 0;
    int overflow = 0;
    int unsafe = 0;

    while ((ch = fgetc(fp)) != EOF) {
        saw_data = 1;
        if (ch == '\n') break;

        unsigned char byte = (unsigned char)ch;
        if ((byte < 32 && byte != '\r') || byte == 127) unsafe = 1;
        if (len + 1 < cap) line[len++] = (char)byte;
        else overflow = 1;
    }

    if (!saw_data && ch == EOF) return 0;

    if (len > 0 && line[len - 1] == '\r') len--;
    for (size_t i = 0; i < len; i++) {
        unsigned char byte = (unsigned char)line[i];
        if (byte < 32 || byte == 127) unsafe = 1;
    }
    line[len] = '\0';

    if (overflow || unsafe) return -1;
    return 1;
}

void rapelz_history_init(void) {
    if (history_loaded) return;

    history_loaded = 1;
    history_count = 0;
    history_index = 0;
    search_index = 0;
    search_query[0] = '\0';
    if (choose_history_path() != 0) return;
    open_history_file();

    if (!history_path[0]) return;

    int read_flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
    read_flags |= O_NOFOLLOW;
#endif
    int read_fd = open(history_path, read_flags);
    if (read_fd < 0) return;

    struct stat read_stat;
    if (fstat(read_fd, &read_stat) != 0 || !S_ISREG(read_stat.st_mode)) {
        close(read_fd);
        return;
    }

    FILE *fp = fdopen(read_fd, "r");
    if (!fp) {
        close(read_fd);
        return;
    }

    char line[RAPELZ_INPUT_SIZE];
    int line_result;
    while ((line_result = read_history_line(fp, line, sizeof(line))) != 0) {
        if (line_result < 0 || line[0] == '\0') continue;

        if (history_count < RAPELZ_HISTORY_MAX) {
            snprintf(history[history_count], RAPELZ_INPUT_SIZE, "%s", line);
            history_count++;
        } else {
            memmove(history, history + 1, (RAPELZ_HISTORY_MAX - 1) * sizeof(history[0]));
            snprintf(history[RAPELZ_HISTORY_MAX - 1], RAPELZ_INPUT_SIZE, "%s", line);
        }
    }
    fclose(fp);
    rapelz_history_reset_navigation();
}

void rapelz_history_shutdown(void) {
    if (history_file) {
        fflush(history_file);
        fclose(history_file);
        history_file = NULL;
    }
    history_loaded = 0;
}

void rapelz_history_add(const char *line) {
    if (!line || !*line) return;

    /*
     * History is stored one command per line. Reject control characters so
     * malformed terminal input cannot create multiline autosuggestions or
     * inject terminal control sequences into the prompt.
     */
    for (const unsigned char *p = (const unsigned char *)line; *p; p++) {
        if (*p < 32 || *p == 127) return;
    }

    if (line[0] == ' ') {
        /* Seperti fish: command yang diawali spasi tidak ditulis ke history. */
        rapelz_history_reset_navigation();
        return;
    }

    if (history_count > 0 && strcmp(history[history_count - 1], line) == 0) {
        rapelz_history_reset_navigation();
        return;
    }

    if (history_count < RAPELZ_HISTORY_MAX) {
        snprintf(history[history_count], RAPELZ_INPUT_SIZE, "%s", line);
        history_count++;
    } else {
        memmove(history, history + 1, (RAPELZ_HISTORY_MAX - 1) * sizeof(history[0]));
        snprintf(history[RAPELZ_HISTORY_MAX - 1], RAPELZ_INPUT_SIZE, "%s", line);
    }

    if (history_file) {
        fprintf(history_file, "%s\n", line);
        fflush(history_file);
    }

    rapelz_history_reset_navigation();
}

void rapelz_history_reset_navigation(void) {
    history_index = history_count;
    rapelz_history_search_reset();
}

int rapelz_history_previous(char *buffer, size_t size) {
    if (!buffer || size == 0 || history_count == 0) return 0;
    if (history_index == 0) return 0;

    history_index--;
    snprintf(buffer, size, "%s", history[history_index]);
    return 1;
}

int rapelz_history_next(char *buffer, size_t size) {
    if (!buffer || size == 0) return 0;
    if (history_index >= history_count) return 0;

    history_index++;
    if (history_index >= history_count) {
        buffer[0] = '\0';
        return 0;
    }

    snprintf(buffer, size, "%s", history[history_index]);
    return 1;
}

void rapelz_history_print(void) {
    for (size_t i = 0; i < history_count; i++)
        printf("%4zu  %s\n", i + 1, history[i]);
}

int rapelz_history_clear(void) {
    history_count = 0;
    rapelz_history_reset_navigation();

    if (history_file) {
        fclose(history_file);
        history_file = NULL;
    }

    /*
     * File bisa terbaca meskipun stream append gagal dibuka. Tetap hapus file
     * saat itu; jika tidak, entri lama akan muncul kembali pada sesi berikutnya.
     */
    if (!history_path[0]) return 0;
    if (unlink(history_path) != 0 && errno != ENOENT) {
        fprintf(stderr, "rapelz: history -c: %s\n", strerror(errno));
        return 1;
    }

    open_history_file();
    return 0;
}

int rapelz_history_find_prefix(const char *prefix, char *buffer, size_t size, size_t *cursor_state) {
    if (!prefix || !buffer || size == 0) return 0;
    size_t prefix_len = strlen(prefix);

    for (size_t i = history_count; i > 0; i--) {
        const char *entry = history[i - 1];
        if (strncmp(entry, prefix, prefix_len) == 0 && strcmp(entry, prefix) != 0) {
            snprintf(buffer, size, "%s", entry);
            if (cursor_state) *cursor_state = strlen(buffer);
            return 1;
        }
    }
    return 0;
}

void rapelz_history_search_reset(void) {
    search_index = history_count;
    search_query[0] = '\0';
}

const char *rapelz_history_search_backward(const char *query) {
    if (!query) return NULL;

    if (strcmp(search_query, query) != 0) {
        snprintf(search_query, sizeof(search_query), "%s", query);
        search_index = history_count;
    }

    for (size_t i = search_index; i > 0; i--) {
        if (strstr(history[i - 1], query) != NULL) {
            search_index = i - 1;
            return history[i - 1];
        }
    }
    return NULL;
}
