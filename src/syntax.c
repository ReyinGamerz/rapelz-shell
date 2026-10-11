#define _POSIX_C_SOURCE 200809L
#include "rapelz.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int command_exists(const char *word) {
    if (!word || !*word) return 0;
    if (strchr(word, '/')) return access(word, X_OK) == 0;

    const char *path = getenv("PATH");
    if (!path) return 0;
    const char *p = path;
    while (1) {
        const char *colon = strchr(p, ':');
        size_t len = colon ? (size_t)(colon - p) : strlen(p);
        char dir[4096];
        if (len == 0) snprintf(dir, sizeof(dir), ".");
        else if (len < sizeof(dir)) {
            memcpy(dir, p, len);
            dir[len] = '\0';
        } else {
            if (!colon) break;
            p = colon + 1;
            continue;
        }

        char full[4096];
        int n = snprintf(full, sizeof(full), "%s/%s", dir, word);
        if (n > 0 && (size_t)n < sizeof(full)) {
            struct stat st;
            if (access(full, X_OK) == 0 && stat(full, &st) == 0 && S_ISREG(st.st_mode)) return 1;
        }
        if (!colon) break;
        p = colon + 1;
    }
    return 0;
}

static int is_operator_char(char c) {
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>';
}

static size_t operator_len(const char *s) {
    if ((s[0] == '|' && (s[1] == '|' || s[1] == '&')) ||
        (s[0] == '&' && s[1] == '&') ||
        (s[0] == '>' && s[1] == '>')) return 2;
    return 1;
}

static int strip_simple_quotes(const char *word, char *out, size_t cap) {
    size_t n = 0;
    int sq = 0, dq = 0;
    for (size_t i = 0; word[i]; i++) {
        char c = word[i];
        if (!dq && c == '\'') { sq = !sq; continue; }
        if (!sq && c == '"') { dq = !dq; continue; }
        if (!sq && !dq && c == '\\' && word[i + 1]) c = word[++i];
        if (n + 1 >= cap) return -1;
        out[n++] = c;
    }
    out[n] = '\0';
    return 0;
}

static void print_word(const char *word, size_t len, int command_position) {
    char tmp[RAPELZ_INPUT_SIZE];
    size_t use = len < sizeof(tmp) ? len : sizeof(tmp) - 1;
    memcpy(tmp, word, use);
    tmp[use] = '\0';

    if (tmp[0] == '#') {
        printf("\033[90m%.*s\033[0m", (int)len, word);
        return;
    }

    if (command_position) {
        char plain[RAPELZ_INPUT_SIZE];
        strip_simple_quotes(tmp, plain, sizeof(plain));
        if (rapelz_alias_exists(plain) || rapelz_builtin_exists(plain)) {
            printf("\033[1;35m%.*s\033[0m", (int)len, word);
        } else if (command_exists(plain)) {
            printf("\033[1;32m%.*s\033[0m", (int)len, word);
        } else {
            printf("\033[1;31m%.*s\033[0m", (int)len, word);
        }
        return;
    }

    if (word[0] == '-') {
        printf("\033[1;33m%.*s\033[0m", (int)len, word);
    } else if (word[0] == '~' || word[0] == '/' || strchr(word, '/') != NULL) {
        printf("\033[1;36m%.*s\033[0m", (int)len, word);
    } else {
        int sq = 0, dq = 0;
        for (size_t i = 0; i < len; i++) {
            if (!dq && word[i] == '\'') sq = !sq;
            else if (!sq && word[i] == '"') dq = !dq;
        }
        if (sq || dq) printf("\033[1;31m%.*s\033[0m", (int)len, word);
        else printf("%.*s", (int)len, word);
    }
}

static void render_actual(const char *buffer) {
    size_t i = 0;
    int command_position = 1;

    while (buffer[i]) {
        if (buffer[i] == ' ' || buffer[i] == '\t') {
            putchar(buffer[i++]);
            continue;
        }

        if (is_operator_char(buffer[i])) {
            size_t n = operator_len(buffer + i);
            char op[4] = {0};
            memcpy(op, buffer + i, n);
            printf("\033[1;34m%.*s\033[0m", (int)n, op);
            if (buffer[i] == '|' || buffer[i] == '&' || buffer[i] == ';') command_position = 1;
            i += n;
            continue;
        }

        size_t start = i;
        int sq = 0, dq = 0;
        while (buffer[i]) {
            char c = buffer[i];
            if (!sq && !dq && (c == ' ' || c == '\t' || is_operator_char(c))) break;
            if (sq) {
                if (c == '\'') sq = 0;
                i++;
                continue;
            }
            if (dq) {
                if (c == '"') dq = 0;
                if (c == '\\' && buffer[i + 1]) i += 2;
                else i++;
                continue;
            }
            if (c == '\'') sq = 1;
            else if (c == '"') dq = 1;
            else if (c == '\\' && buffer[i + 1]) i++;
            i++;
        }
        print_word(buffer + start, i - start, command_position);
        command_position = 0;
    }
}

void rapelz_render_line(const char *buffer, size_t cursor) {
    if (!buffer) return;
    render_actual(buffer);

    size_t length = strlen(buffer);
    if (cursor == length) {
        const char *suggestion = rapelz_suggestion_for(buffer);
        if (suggestion && strlen(suggestion) > length &&
            strncmp(suggestion, buffer, length) == 0) {
            printf("\033[90m%s\033[0m", suggestion + length);
        }
    }
    fflush(stdout);
}

void rapelz_render_line_final(const char *buffer) {
    if (!buffer) return;
    render_actual(buffer);
    fflush(stdout);
}
