#define _POSIX_C_SOURCE 200809L
#include "rapelz.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char suggestion[RAPELZ_SUGGESTION_MAX];

static int starts_with(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static int path_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void expand_home(const char *input, char *output, size_t size) {
    const char *home = getenv("HOME");
    if (home && (strcmp(input, "~") == 0 || strncmp(input, "~/", 2) == 0))
        snprintf(output, size, "%s%s", home, input + 1);
    else
        snprintf(output, size, "%s", input);
}

static int suggestion_is_safe_history_command(const char *text) {
    if (!text || !*text) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        /* Control characters can move the terminal cursor or inject fake lines. */
        if (*p < 32 || *p == 127) return 0;
    }

    /*
     * A corrupted history entry may contain a copied prompt or terminal
     * transcript. Never show those as inline command suggestions.
     */
    if (strstr(text, "╭─") || strstr(text, "╰─") ||
        strstr(text, "❯") || strstr(text, "RAPELZ Shell") ||
        strstr(text, " $ ") || strstr(text, "make test")) return 0;

    return 1;
}

static const char *history_suggestion(const char *buffer) {
    size_t state = (size_t)-1;
    if (rapelz_history_find_prefix(buffer, suggestion, sizeof(suggestion), &state) &&
        suggestion_is_safe_history_command(suggestion))
        return suggestion;
    return NULL;
}

static int command_position(const char *buffer, size_t token_start) {
    size_t i = token_start;
    while (i > 0) {
        i--;
        if (buffer[i] == '|' || buffer[i] == '&' || buffer[i] == ';') {
            if (i > 0 && ((buffer[i] == '|' && buffer[i - 1] == '|') ||
                          (buffer[i] == '&' && buffer[i - 1] == '&'))) return 1;
            return 1;
        }
        if (buffer[i] != ' ' && buffer[i] != '\t') return 0;
    }
    return 1;
}

static const char *command_suggestion(const char *buffer) {
    size_t len = strlen(buffer);
    size_t start = len;
    while (start > 0 && buffer[start - 1] != ' ' && buffer[start - 1] != '\t') start--;
    if (!command_position(buffer, start)) return NULL;

    const char *prefix = buffer + start;
    if (!*prefix || strchr(prefix, '/') || prefix[0] == '~') return NULL;

    if (rapelz_alias_exists(prefix)) {
        snprintf(suggestion, sizeof(suggestion), "%s", prefix);
        return suggestion;
    }

    static const char *const builtins[] = {
        "alias", "bg", "cd", "command", "eval", "exit", "false", "fg",
        "help", "history", "jobs", "pwd", "set", "source", "true", "type",
        "unalias", "unset", "wait", "export"
    };
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        if (starts_with(builtins[i], prefix)) {
            snprintf(suggestion, sizeof(suggestion), "%s", builtins[i]);
            return suggestion;
        }
    }

    const char *path = getenv("PATH");
    if (!path) return NULL;

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
                if (access(full, X_OK) == 0 && !path_is_dir(full)) {
                    snprintf(suggestion, sizeof(suggestion), "%s", entry->d_name);
                    closedir(dp);
                    return suggestion;
                }
            }
            closedir(dp);
        }
        if (!colon) break;
        p = colon + 1;
    }
    return NULL;
}

static const char *path_suggestion(const char *buffer) {
    size_t len = strlen(buffer);
    size_t start = len;
    while (start > 0 && buffer[start - 1] != ' ' && buffer[start - 1] != '\t') start--;
    const char *token = buffer + start;
    if (!*token || token[0] == '-' || token[0] == '$') return NULL;

    char expanded[RAPELZ_INPUT_SIZE];
    expand_home(token, expanded, sizeof(expanded));

    const char *slash = strrchr(expanded, '/');
    char dir[RAPELZ_INPUT_SIZE];
    char base[RAPELZ_INPUT_SIZE];

    if (slash) {
        size_t dir_len = (size_t)(slash - expanded) + 1;
        if (dir_len >= sizeof(dir)) return NULL;
        memcpy(dir, expanded, dir_len);
        dir[dir_len] = '\0';
        snprintf(base, sizeof(base), "%s", slash + 1);
    } else {
        snprintf(dir, sizeof(dir), "./");
        snprintf(base, sizeof(base), "%s", expanded);
    }

    char real_dir[RAPELZ_INPUT_SIZE];
    expand_home(dir, real_dir, sizeof(real_dir));
    DIR *dp = opendir(real_dir);
    if (!dp) return NULL;

    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (!starts_with(entry->d_name, base)) continue;
        if (entry->d_name[0] == '.' && (base[0] != '.' || entry->d_name[1] == '\0')) continue;

        char full[RAPELZ_INPUT_SIZE];
        int n = snprintf(full, sizeof(full), "%s%s", real_dir, entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(full)) continue;

        int is_dir = path_is_dir(full);
        if (!is_dir && access(full, R_OK) != 0) continue;

        if (token[0] == '~' && !strrchr(token, '/')) {
            snprintf(suggestion, sizeof(suggestion), "~/%s%s", entry->d_name, is_dir ? "/" : "");
        } else if (slash || strchr(token, '/') != NULL) {
            size_t slash_pos = 0;
            const char *token_slash = strrchr(token, '/');
            if (token_slash) slash_pos = (size_t)(token_slash - token) + 1;
            snprintf(suggestion, sizeof(suggestion), "%.*s%s%s",
                     (int)slash_pos, token, entry->d_name, is_dir ? "/" : "");
        } else {
            snprintf(suggestion, sizeof(suggestion), "%s%s%s",
                     token, entry->d_name + strlen(base), is_dir ? "/" : "");
        }
        closedir(dp);
        return suggestion;
    }

    closedir(dp);
    return NULL;
}

const char *rapelz_suggestion_for(const char *buffer) {
    if (!buffer || !*buffer) return NULL;
    suggestion[0] = '\0';

    const char *history = history_suggestion(buffer);
    if (history && strcmp(history, buffer) != 0) return history;

    const char *command = command_suggestion(buffer);
    if (command && strlen(command) > 0 && strcmp(command, buffer) != 0) return command;

    const char *path = path_suggestion(buffer);
    if (path && strlen(path) > strlen(buffer) && starts_with(path, buffer)) return path;
    return NULL;
}
