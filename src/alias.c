#define _POSIX_C_SOURCE 200809L
#include "rapelz.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

struct alias_entry {
    char name[RAPELZ_ALIAS_NAME_MAX];
    char value[RAPELZ_ALIAS_VALUE_MAX];
};

static struct alias_entry aliases[RAPELZ_MAX_ALIASES];
static size_t alias_count;

void rapelz_alias_init(void) {
    alias_count = 0;
}

void rapelz_alias_shutdown(void) {
    alias_count = 0;
}

static int alias_index(const char *name) {
    if (!name) return -1;
    for (size_t i = 0; i < alias_count; i++) {
        if (strcmp(aliases[i].name, name) == 0) return (int)i;
    }
    return -1;
}

int rapelz_alias_set(const char *name, const char *value) {
    if (!name || !*name || !value) return -1;
    if (strlen(name) >= RAPELZ_ALIAS_NAME_MAX ||
        strlen(value) >= RAPELZ_ALIAS_VALUE_MAX) return -1;

    for (const char *p = name; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) return -1;
    }

    int index = alias_index(name);
    if (index < 0) {
        if (alias_count >= RAPELZ_MAX_ALIASES) return -1;
        index = (int)alias_count++;
    }

    snprintf(aliases[index].name, sizeof(aliases[index].name), "%s", name);
    snprintf(aliases[index].value, sizeof(aliases[index].value), "%s", value);
    return 0;
}

int rapelz_alias_unset(const char *name) {
    int index = alias_index(name);
    if (index < 0) return -1;

    for (size_t i = (size_t)index; i + 1 < alias_count; i++)
        aliases[i] = aliases[i + 1];
    alias_count--;
    return 0;
}

const char *rapelz_alias_get(const char *name) {
    int index = alias_index(name);
    return index >= 0 ? aliases[index].value : NULL;
}

int rapelz_alias_exists(const char *name) {
    return alias_index(name) >= 0;
}

void rapelz_alias_print(void) {
    for (size_t i = 0; i < alias_count; i++)
        printf("alias %s='%s'\n", aliases[i].name, aliases[i].value);
}

size_t rapelz_alias_count(void) {
    return alias_count;
}

const char *rapelz_alias_name_at(size_t index) {
    if (index >= alias_count) return NULL;
    return aliases[index].name;
}

static int is_operator(char c) {
    return c == '|' || c == '&' || c == ';';
}

static int is_redirection(char c) {
    return c == '<' || c == '>';
}

/* Expand one alias occurrence at a command position. */
static int expand_one_alias(const char *line, char *out, size_t size) {
    int command_position = 1;
    int redir_target = 0;
    int sq = 0;
    int dq = 0;
    int escaped = 0;

    for (size_t i = 0; line[i]; ) {
        char c = line[i];

        if (escaped) {
            escaped = 0;
            i++;
            continue;
        }
        if (!sq && c == '\\') {
            escaped = 1;
            i++;
            continue;
        }
        if (!dq && c == '\'') {
            sq = !sq;
            i++;
            continue;
        }
        if (!sq && c == '"') {
            dq = !dq;
            i++;
            continue;
        }

        if (sq || dq) {
            i++;
            continue;
        }

        if (isspace((unsigned char)c)) {
            i++;
            continue;
        }

        if (is_operator(c)) {
            command_position = 1;
            redir_target = 0;
            i++;
            continue;
        }

        if (is_redirection(c)) {
            redir_target = 1;
            i++;
            if (c == '>' && line[i] == '>') i++;
            if (line[i] == '&') i++;
            continue;
        }

        if (redir_target) {
            size_t j = i;
            int tsq = 0, tdq = 0, tescaped = 0;
            while (line[j]) {
                char x = line[j];
                if (tescaped) { tescaped = 0; j++; continue; }
                if (!tsq && x == '\\') { tescaped = 1; j++; continue; }
                if (!tdq && x == '\'') { tsq = !tsq; j++; continue; }
                if (!tsq && x == '"') { tdq = !tdq; j++; continue; }
                if (!tsq && !tdq && (isspace((unsigned char)x) || is_operator(x) || is_redirection(x))) break;
                j++;
            }
            redir_target = 0;
            i = j;
            continue;
        }

        if (!command_position) {
            while (line[i] && !isspace((unsigned char)line[i]) &&
                   !is_operator(line[i]) && !is_redirection(line[i]))
                i++;
            continue;
        }

        size_t start = i;
        while (line[i] && !isspace((unsigned char)line[i]) &&
               !is_operator(line[i]) && !is_redirection(line[i]))
            i++;
        size_t len = i - start;
        if (len == 0) continue;

        if (len >= RAPELZ_ALIAS_NAME_MAX) {
            command_position = 0;
            continue;
        }

        if (line[start] == '\'' || line[start] == '"' || line[start] == '\\') {
            command_position = 0;
            continue;
        }

        char name[RAPELZ_ALIAS_NAME_MAX];
        memcpy(name, line + start, len);
        name[len] = '\0';
        const char *value = rapelz_alias_get(name);
        if (!value) {
            command_position = 0;
            continue;
        }

        size_t prefix_len = start;
        size_t suffix_len = strlen(line + i);
        size_t value_len = strlen(value);
        if (prefix_len + value_len + suffix_len + 1 > size) return -2;

        memcpy(out, line, prefix_len);
        memcpy(out + prefix_len, value, value_len);
        memcpy(out + prefix_len + value_len, line + i, suffix_len + 1);
        return 1;
    }

    if (strlen(line) + 1 > size) return -2;
    memcpy(out, line, strlen(line) + 1);
    return 0;
}

int rapelz_alias_expand_line(const char *line, char *out, size_t size) {
    if (!line || !out || size == 0) return -1;

    char current[RAPELZ_INPUT_SIZE];
    char next[RAPELZ_INPUT_SIZE];
    if (strlen(line) >= sizeof(current)) {
        fprintf(stderr, "rapelz: baris terlalu panjang\n");
        return -1;
    }
    memcpy(current, line, strlen(line) + 1);

    int expanded_any = 0;
    int stabilized = 0;
    for (int pass = 0; pass < 64; pass++) {
        int rc = expand_one_alias(current, next, sizeof(next));
        if (rc == -2) {
            fprintf(stderr, "rapelz: alias menghasilkan baris terlalu panjang\n");
            return -1;
        }
        if (rc < 0) return -1;
        if (rc == 0 || strcmp(next, current) == 0) {
            stabilized = 1;
            break;
        }
        expanded_any = 1;
        memcpy(current, next, strlen(next) + 1);
    }

    if (!stabilized && expanded_any) {
        fprintf(stderr, "rapelz: ekspansi alias terlalu dalam (mungkin ada alias berputar)\n");
        return -1;
    }

    if (strlen(current) >= size) return -1;
    memcpy(out, current, strlen(current) + 1);
    return 0;
}
