#ifndef RAPELZ_H
#define RAPELZ_H

#include <stddef.h>
#include <sys/types.h>

#define RAPELZ_VERSION "0.9"
#define RAPELZ_INPUT_SIZE 4096
#define RAPELZ_MAX_ARGS 256
#define RAPELZ_HISTORY_MAX 500
#define RAPELZ_COMPLETION_MAX 256
#define RAPELZ_COMPLETION_NAME_MAX 512
#define RAPELZ_SUGGESTION_MAX RAPELZ_INPUT_SIZE
#define RAPELZ_MAX_REDIRS 32
#define RAPELZ_MAX_PIPE_COMMANDS 32
#define RAPELZ_MAX_PIPELINES 64
#define RAPELZ_MAX_JOBS 32
#define RAPELZ_MAX_ALIASES 128
#define RAPELZ_ALIAS_NAME_MAX 128
#define RAPELZ_ALIAS_VALUE_MAX RAPELZ_INPUT_SIZE

/* ---------- Parser ---------- */
typedef enum {
    RAPELZ_CONN_NONE = 0,
    RAPELZ_CONN_SEMI,
    RAPELZ_CONN_AND,
    RAPELZ_CONN_OR
} rapelz_connector_t;

typedef enum {
    RAPELZ_REDIR_IN = 0,
    RAPELZ_REDIR_OUT,
    RAPELZ_REDIR_APPEND,
    RAPELZ_REDIR_ERR_OUT,
    RAPELZ_REDIR_ERR_APPEND,
    RAPELZ_REDIR_DUP_OUT,
    RAPELZ_REDIR_DUP_IN,
    RAPELZ_REDIR_CLOSE
} rapelz_redir_type_t;

typedef struct {
    int fd;
    rapelz_redir_type_t type;
    char *target;
    int dup_fd;
} rapelz_redirection_t;

typedef struct {
    char *argv[RAPELZ_MAX_ARGS];
    int argc;
    int pipe_stderr;
    rapelz_redirection_t redirs[RAPELZ_MAX_REDIRS];
    int redir_count;
} rapelz_command_t;

typedef struct {
    rapelz_command_t commands[RAPELZ_MAX_PIPE_COMMANDS];
    int command_count;
    int background;
} rapelz_pipeline_t;

typedef struct {
    rapelz_pipeline_t pipelines[RAPELZ_MAX_PIPELINES];
    rapelz_connector_t connectors[RAPELZ_MAX_PIPELINES];
    int pipeline_count;
} rapelz_program_t;

int rapelz_parse_line(const char *line, rapelz_program_t *program);
int rapelz_expand_command(rapelz_command_t *command);
void rapelz_free_program(rapelz_program_t *program);

/* ---------- Prompt ---------- */
void rapelz_print_prompt(void);

/* ---------- Line editor ---------- */
int rapelz_readline(char *buffer, size_t size);
void rapelz_render_line(const char *buffer, size_t cursor);
void rapelz_render_line_final(const char *buffer);
void rapelz_input_shutdown(void);

/* ---------- History ---------- */
void rapelz_history_init(void);
void rapelz_history_shutdown(void);
void rapelz_history_add(const char *line);
int rapelz_history_previous(char *buffer, size_t size);
int rapelz_history_next(char *buffer, size_t size);
void rapelz_history_reset_navigation(void);
void rapelz_history_print(void);
int rapelz_history_clear(void);
int rapelz_history_find_prefix(const char *prefix, char *buffer, size_t size, size_t *cursor_state);
void rapelz_history_search_reset(void);
const char *rapelz_history_search_backward(const char *query);

/* ---------- Completion / suggestions ---------- */
int rapelz_complete(char *buffer, size_t *length, size_t *cursor, size_t size);
void rapelz_completion_print(const char *buffer, size_t cursor);
const char *rapelz_suggestion_for(const char *buffer);

/* ---------- Aliases / config ---------- */
void rapelz_alias_init(void);
void rapelz_alias_shutdown(void);
int rapelz_alias_set(const char *name, const char *value);
int rapelz_alias_unset(const char *name);
const char *rapelz_alias_get(const char *name);
void rapelz_alias_print(void);
int rapelz_alias_exists(const char *name);
size_t rapelz_alias_count(void);
const char *rapelz_alias_name_at(size_t index);
int rapelz_alias_expand_line(const char *line, char *out, size_t size);
void rapelz_config_load(void);

/* ---------- Shell ---------- */
void rapelz_shell_init(void);
void rapelz_shell_shutdown(void);
void rapelz_install_signals(void);
int rapelz_run_line(char *line);
int rapelz_last_status(void);
void rapelz_set_last_status(int status);
int rapelz_pipefail_enabled(void);
void rapelz_set_pipefail(int enabled);
int rapelz_execute_script_file(const char *path);
int rapelz_capture_command(const char *line, char **output, size_t *output_len);
int rapelz_builtin_exists(const char *name);
void rapelz_jobs_reap(void);

#endif
