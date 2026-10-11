#define _POSIX_C_SOURCE 200809L
#include "rapelz.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char names[RAPELZ_COMPLETION_MAX][RAPELZ_COMPLETION_NAME_MAX];
    size_t count;
} completion_list;

static int starts_with(const char *name, const char *prefix) {
    return strncmp(name, prefix, strlen(prefix)) == 0;
}

static void add_candidate(completion_list *list, const char *name) {
    if (!list || !name || !*name || list->count >= RAPELZ_COMPLETION_MAX) return;
    for (size_t i = 0; i < list->count; i++)
        if (strcmp(list->names[i], name) == 0) return;
    snprintf(list->names[list->count], sizeof(list->names[list->count]), "%s", name);
    list->count++;
}

static int is_directory(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void add_builtins(completion_list *list, const char *prefix) {
    static const char *const builtins[] = {
        ".", "alias", "bg", "cd", "command", "eval", "exit", "false", "fg",
        "help", "history", "jobs", "pwd", "set", "source", "true", "type",
        "unalias", "unset", "wait", "export", "dirs", "pushd", "popd"
    };
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        if (starts_with(builtins[i], prefix)) add_candidate(list, builtins[i]);
}

static void add_aliases(completion_list *list, const char *prefix) {
    size_t count = rapelz_alias_count();
    for (size_t i = 0; i < count; i++) {
        const char *name = rapelz_alias_name_at(i);
        if (name && starts_with(name, prefix)) add_candidate(list, name);
    }
}

static void complete_commands(const char *prefix, completion_list *list) {
    add_builtins(list, prefix);
    add_aliases(list, prefix);

    const char *path = getenv("PATH");
    if (!path) return;

    const char *p = path;
    while (1) {
        const char *colon = strchr(p, ':');
        size_t dir_len = colon ? (size_t)(colon - p) : strlen(p);
        char dir[RAPELZ_INPUT_SIZE];
        if (dir_len == 0) snprintf(dir, sizeof(dir), ".");
        else if (dir_len < sizeof(dir)) {
            memcpy(dir, p, dir_len);
            dir[dir_len] = '\0';
        } else {
            if (!colon) break;
            p = colon + 1;
            continue;
        }

        DIR *dp = opendir(dir);
        if (dp) {
            struct dirent *entry;
            while ((entry = readdir(dp)) != NULL) {
                if (!starts_with(entry->d_name, prefix)) continue;
                if (entry->d_name[0] == '.' && prefix[0] != '.') continue;
                char full[4096];
                int n = snprintf(full, sizeof(full), "%s/%s", dir, entry->d_name);
                if (n <= 0 || (size_t)n >= sizeof(full)) continue;
                if (access(full, X_OK) == 0 && !is_directory(full))
                    add_candidate(list, entry->d_name);
            }
            closedir(dp);
        }

        if (!colon) break;
        p = colon + 1;
    }
}

static void add_env_names(const char *prefix, completion_list *list, int dollar) {
    extern char **environ;
    for (char **p = environ; p && *p; p++) {
        const char *eq = strchr(*p, '=');
        if (!eq) continue;
        char name[256];
        size_t len = (size_t)(eq - *p);
        if (len == 0 || len >= sizeof(name)) continue;
        memcpy(name, *p, len);
        name[len] = '\0';
        if (!starts_with(name, prefix)) continue;

        char candidate[260];
        if (dollar) snprintf(candidate, sizeof(candidate), "$%s", name);
        else snprintf(candidate, sizeof(candidate), "%s", name);
        add_candidate(list, candidate);
    }
}

static int is_assignment_word(const char *word) {
    if (!word || !*word) return 0;
    const char *eq = strchr(word, '=');
    if (!eq || eq == word) return 0;
    if (!(isalpha((unsigned char)word[0]) || word[0] == '_')) return 0;
    for (const char *p = word + 1; p < eq; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_')) return 0;
    }
    return 1;
}

static int command_name_before(const char *buffer, size_t before,
                               char *name, size_t name_size) {
    size_t i = 0;
    int have_command = 0;

    while (i < before) {
        while (i < before && (buffer[i] == ' ' || buffer[i] == '\t')) i++;
        if (i >= before) break;

        if (buffer[i] == '|' || buffer[i] == '&' || buffer[i] == ';') {
            if (buffer[i] == '|' && i + 1 < before && buffer[i + 1] == '|') i += 2;
            else if (buffer[i] == '&' && i + 1 < before && buffer[i + 1] == '&') i += 2;
            else if (buffer[i] == '|' && i + 1 < before && buffer[i + 1] == '&') i += 2;
            else i++;
            have_command = 0;
            continue;
        }

        size_t save = i;
        while (i < before && isdigit((unsigned char)buffer[i])) i++;
        if (i < before && (buffer[i] == '<' || buffer[i] == '>')) {
            i++;
            if (i < before && buffer[i] == '>') i++;
            if (i < before && buffer[i] == '&') {
                i++;
                if (i < before && (isdigit((unsigned char)buffer[i]) || buffer[i] == '-')) {
                    while (i < before && isdigit((unsigned char)buffer[i])) i++;
                    if (i < before && buffer[i] == '-') i++;
                    continue;
                }
            }
            while (i < before && (buffer[i] == ' ' || buffer[i] == '\t')) i++;
            while (i < before && buffer[i] != ' ' && buffer[i] != '\t' &&
                   buffer[i] != '|' && buffer[i] != '&' && buffer[i] != ';') i++;
            continue;
        }
        i = save;

        size_t start = i;
        int sq = 0, dq = 0, escaped = 0;
        while (i < before) {
            char c = buffer[i];
            if (escaped) { escaped = 0; i++; continue; }
            if (!sq && c == '\\') { escaped = 1; i++; continue; }
            if (!dq && c == '\'') { sq = !sq; i++; continue; }
            if (!sq && c == '"') { dq = !dq; i++; continue; }
            if (!sq && !dq && (c == ' ' || c == '\t' || c == '|' || c == '&' || c == ';' || c == '<' || c == '>')) break;
            i++;
        }
        if (i == start) { i++; continue; }

        char word[512];
        size_t len = i - start;
        if (len >= sizeof(word)) len = sizeof(word) - 1;
        memcpy(word, buffer + start, len);
        word[len] = '\0';
        if (!have_command && !is_assignment_word(word)) {
            if (name && name_size) snprintf(name, name_size, "%s", word);
            have_command = 1;
        }
    }
    return have_command;
}

static void expand_home(const char *input, char *output, size_t size) {
    const char *home = getenv("HOME");
    if (home && (strcmp(input, "~") == 0 || strncmp(input, "~/", 2) == 0))
        snprintf(output, size, "%s%s", home, input + 1);
    else
        snprintf(output, size, "%s", input);
}

static void complete_paths(const char *prefix, completion_list *list) {
    char expanded[RAPELZ_INPUT_SIZE];
    char dir[RAPELZ_INPUT_SIZE];
    char base[RAPELZ_INPUT_SIZE];
    const char *slash = NULL;

    if (strcmp(prefix, "~") == 0) {
        const char *home = getenv("HOME");
        if (!home || !*home) return;
        snprintf(expanded, sizeof(expanded), "%s/", home);
        snprintf(dir, sizeof(dir), "%s/", home);
        base[0] = '\0';
    } else {
        expand_home(prefix, expanded, sizeof(expanded));
        slash = strrchr(expanded, '/');

        if (slash) {
            size_t dir_len = (size_t)(slash - expanded) + 1;
            if (dir_len >= sizeof(dir)) return;
            memcpy(dir, expanded, dir_len);
            dir[dir_len] = '\0';
            snprintf(base, sizeof(base), "%s", slash + 1);
        } else {
            snprintf(dir, sizeof(dir), "./");
            snprintf(base, sizeof(base), "%s", expanded);
        }
    }

    char real_dir[RAPELZ_INPUT_SIZE];
    expand_home(dir, real_dir, sizeof(real_dir));
    DIR *dp = opendir(real_dir);
    if (!dp) return;

    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (!starts_with(entry->d_name, base)) continue;
        if (entry->d_name[0] == '.' && (base[0] != '.' || entry->d_name[1] == '\0')) continue;

        char full[RAPELZ_INPUT_SIZE];
        int n = snprintf(full, sizeof(full), "%s%s", real_dir, entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(full)) continue;

        int dir_match = is_directory(full);
        char candidate[RAPELZ_COMPLETION_NAME_MAX];
        const char *slash_in_prefix = strrchr(prefix, '/');

        if (prefix[0] == '~' && !slash_in_prefix) {
            snprintf(candidate, sizeof(candidate), "~/%s%s", entry->d_name, dir_match ? "/" : "");
        } else if (slash_in_prefix) {
            size_t prefix_dir_len = (size_t)(slash_in_prefix - prefix) + 1;
            snprintf(candidate, sizeof(candidate), "%.*s%s%s",
                     (int)prefix_dir_len, prefix, entry->d_name, dir_match ? "/" : "");
        } else {
            snprintf(candidate, sizeof(candidate), "%s%s", entry->d_name, dir_match ? "/" : "");
        }
        add_candidate(list, candidate);
    }
    closedir(dp);
}

static int compare_candidates(const void *a, const void *b) {
    const char *left = (const char *)a;
    const char *right = (const char *)b;
    return strcmp(left, right);
}

static void sort_candidates(completion_list *list) {
    qsort(list->names, list->count, sizeof(list->names[0]), compare_candidates);
}

static size_t common_prefix_length(const completion_list *list) {
    if (!list || list->count == 0) return 0;
    size_t n = strlen(list->names[0]);
    for (size_t i = 1; i < list->count; i++) {
        size_t j = 0;
        while (j < n && list->names[0][j] == list->names[i][j]) j++;
        n = j;
    }
    return n;
}

static int is_shell_operator(char c) {
    return c == '|' || c == '&' || c == ';' || c == '<' || c == '>';
}

static size_t token_start(const char *buffer, size_t cursor) {
    size_t start = 0;
    int sq = 0, dq = 0, escaped = 0;

    for (size_t i = 0; i < cursor; i++) {
        char c = buffer[i];
        if (escaped) { escaped = 0; continue; }
        if (!sq && c == '\\') { escaped = 1; continue; }
        if (!dq && c == '\'') { sq = !sq; continue; }
        if (!sq && c == '"') { dq = !dq; continue; }
        if (!sq && !dq && (c == ' ' || c == '\t' || is_shell_operator(c)))
            start = i + 1;
    }
    return start;
}

static int in_command_position(const char *buffer, size_t start) {
    size_t i = start;
    while (i > 0 && (buffer[i - 1] == ' ' || buffer[i - 1] == '\t')) i--;
    if (i == 0) return 1;
    char prev = buffer[i - 1];
    return prev == '|' || prev == '&' || prev == ';';
}

static void collect_for_buffer(const char *buffer, size_t cursor, completion_list *list) {
    size_t start = token_start(buffer, cursor);
    size_t token_len = cursor - start;
    char token[RAPELZ_INPUT_SIZE];
    if (token_len >= sizeof(token)) return;
    memcpy(token, buffer + start, token_len);
    token[token_len] = '\0';

    if (token[0] == '$') {
        completion_list env = {0};
        add_env_names(token + 1, &env, 1);
        for (size_t i = 0; i < env.count; i++) add_candidate(list, env.names[i]);
        return;
    }

    if (in_command_position(buffer, start) && !strchr(token, '/') && token[0] != '~') {
        complete_commands(token, list);
        return;
    }

    char command[512] = {0};
    if (command_name_before(buffer, start, command, sizeof(command))) {
        if (strcmp(command, "export") == 0 || strcmp(command, "unset") == 0) {
            completion_list env = {0};
            add_env_names(token, &env, 0);
            for (size_t i = 0; i < env.count; i++) add_candidate(list, env.names[i]);
            return;
        }
        if (strcmp(command, "unalias") == 0 || strcmp(command, "alias") == 0) {
            size_t count = rapelz_alias_count();
            for (size_t i = 0; i < count; i++) {
                const char *name = rapelz_alias_name_at(i);
                if (name && starts_with(name, token)) add_candidate(list, name);
            }
            if (strcmp(command, "unalias") == 0) return;
        }
    }

    complete_paths(token, list);
}

int rapelz_complete(char *buffer, size_t *length, size_t *cursor, size_t size) {
    if (!buffer || !length || !cursor || size < 2 || *length >= size || *cursor > *length)
        return 0;

    size_t start = token_start(buffer, *cursor);
    size_t token_len = *cursor - start;
    char token[RAPELZ_INPUT_SIZE];
    if (token_len >= sizeof(token)) return 0;
    memcpy(token, buffer + start, token_len);
    token[token_len] = '\0';

    completion_list list = {0};
    collect_for_buffer(buffer, *cursor, &list);
    if (!list.count) return 0;

    sort_candidates(&list);
    size_t common = common_prefix_length(&list);
    if (common <= token_len) return 2;

    size_t replacement_len = common;
    if (replacement_len >= RAPELZ_COMPLETION_NAME_MAX)
        replacement_len = RAPELZ_COMPLETION_NAME_MAX - 1;

    size_t suffix = replacement_len - token_len;
    if (*length + suffix >= size) suffix = size - *length - 1;
    if (!suffix) return 2;

    memmove(buffer + *cursor + suffix, buffer + *cursor, *length - *cursor);
    memcpy(buffer + *cursor, list.names[0] + token_len, suffix);
    *cursor += suffix;
    *length += suffix;
    buffer[*length] = '\0';

    if (list.count == 1 && *cursor == *length) {
        size_t full_len = strlen(list.names[0]);
        int is_dir = full_len > 0 && list.names[0][full_len - 1] == '/';
        int command = in_command_position(buffer, start) && !strchr(list.names[0], '/');
        if (!is_dir && command && *length + 1 < size && buffer[*length - 1] != ' ') {
            buffer[*length] = ' ';
            (*length)++;
            (*cursor)++;
            buffer[*length] = '\0';
        }
    }

    return 1;
}

void rapelz_completion_print(const char *buffer, size_t cursor) {
    if (!buffer || cursor > strlen(buffer)) return;

    completion_list list = {0};
    collect_for_buffer(buffer, cursor, &list);
    if (!list.count) return;
    sort_candidates(&list);

    size_t width = 0;
    for (size_t i = 0; i < list.count; i++) {
        size_t len = strlen(list.names[i]);
        if (len > width) width = len;
    }
    width += 2;

    for (size_t i = 0; i < list.count; i++) {
        printf("%-*s", (int)width, list.names[i]);
        if ((i + 1) % 4 == 0 || i + 1 == list.count) putchar('\n');
    }
    fflush(stdout);
}
