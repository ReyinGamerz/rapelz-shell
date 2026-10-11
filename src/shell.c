#define _POSIX_C_SOURCE 200809L
#include "rapelz.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

typedef struct {
    int used;
    int id;
    pid_t pgid;
    pid_t pids[RAPELZ_MAX_PIPE_COMMANDS];
    int count;
    int state; /* 1=berjalan, 2=berhenti, 3=selesai dan menunggu wait */
    int status;
    int statuses[RAPELZ_MAX_PIPE_COMMANDS];
    int pipefail;
    char command[RAPELZ_INPUT_SIZE];
} job_t;

static job_t jobs[RAPELZ_MAX_JOBS];
static int next_job_id = 1;
static int shell_interactive;
static int shell_job_control;
static pid_t shell_pgid;
static struct termios shell_tmodes;
static int last_status_value;
static int pipefail_option;
static int capture_depth;

#define RAPELZ_DIR_STACK_MAX 32
static char directory_stack[RAPELZ_DIR_STACK_MAX][4096];
static size_t directory_stack_count;

static void write_best_effort(int fd, const void *buf, size_t len) {
    ssize_t result = write(fd, buf, len);
    (void)result;
}
static int status_of_pid(pid_t pid, int *st_out);
static void pipeline_text(const rapelz_pipeline_t *pl, char *out, size_t cap);
static int run_job_builtin(rapelz_command_t *cmd);

static int set_sig(int sig, void (*handler)(int), int flags) {
    struct sigaction a;
    memset(&a, 0, sizeof(a));
    sigemptyset(&a.sa_mask);
    a.sa_handler = handler;
    a.sa_flags = flags;
    return sigaction(sig, &a, NULL);
}

static int wait_status(int st) {
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    if (WIFSTOPPED(st)) return 128 + WSTOPSIG(st);
    return 1;
}

/* Semua jalur penantian child harus tahan terhadap sinyal yang menyela waitpid. */
static pid_t waitpid_nointr(pid_t pid, int *status, int options) {
    pid_t result;
    do {
        result = waitpid(pid, status, options);
    } while (result < 0 && errno == EINTR);
    return result;
}

/*
 * Tentukan status pipeline dengan aturan yang sama pada wait, fg, dan job
 * reaper. pipefail mengambil status nonnol paling kanan, bukan proses terakhir
 * yang kebetulan selesai ditunggu oleh loop.
 */
static int pipeline_status(const int *statuses, int count, int pipefail) {
    if (!statuses || count <= 0) return 1;

    if (pipefail) {
        for (int i = count - 1; i >= 0; i--) {
            if (statuses[i] >= 0 && statuses[i] != 0) return statuses[i];
        }
    }

    return statuses[count - 1] >= 0 ? statuses[count - 1] : 1;
}

int rapelz_last_status(void) { return last_status_value; }
void rapelz_set_last_status(int status) { last_status_value = status < 0 ? 1 : status & 255; }
int rapelz_pipefail_enabled(void) { return pipefail_option; }
void rapelz_set_pipefail(int enabled) { pipefail_option = enabled ? 1 : 0; }

static void sigint_prompt(int sig) {
    (void)sig;
    const char nl = '\n';
    write_best_effort(STDOUT_FILENO, &nl, 1);
}

void rapelz_install_signals(void) {
    if (shell_interactive) {
        set_sig(SIGINT, sigint_prompt, 0);
        set_sig(SIGQUIT, SIG_IGN, 0);
        set_sig(SIGTSTP, SIG_IGN, 0);
        set_sig(SIGTTIN, SIG_IGN, 0);
        set_sig(SIGTTOU, SIG_IGN, 0);
    } else {
        set_sig(SIGINT, SIG_DFL, 0);
        set_sig(SIGQUIT, SIG_DFL, 0);
        set_sig(SIGTSTP, SIG_DFL, 0);
    }
}

static void restore_child_signals(void) {
    set_sig(SIGINT, SIG_DFL, 0);
    set_sig(SIGQUIT, SIG_DFL, 0);
    set_sig(SIGTSTP, SIG_DFL, 0);
    set_sig(SIGTTIN, SIG_DFL, 0);
    set_sig(SIGTTOU, SIG_DFL, 0);
    set_sig(SIGCHLD, SIG_DFL, 0);
}

static void init_job_control(void) {
    shell_interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    shell_job_control = 0;
    if (!shell_interactive) return;

    shell_pgid = getpid();
    if (setpgid(shell_pgid, shell_pgid) < 0 && errno != EACCES) {
        shell_interactive = 0;
        return;
    }
    if (tcsetpgrp(STDIN_FILENO, shell_pgid) != 0) {
        shell_interactive = 0;
        return;
    }
    tcgetattr(STDIN_FILENO, &shell_tmodes);
    shell_job_control = 1;
}

void rapelz_shell_init(void) {
    memset(jobs, 0, sizeof(jobs));
    next_job_id = 1;
    last_status_value = 0;
    pipefail_option = 0;
    capture_depth = 0;
    directory_stack_count = 0;
    memset(directory_stack, 0, sizeof(directory_stack));
    init_job_control();
    rapelz_install_signals();
}

static void terminate_jobs(void) {
    for (int i = 0; i < RAPELZ_MAX_JOBS; i++) {
        if (!jobs[i].used) continue;
        if (jobs[i].pgid > 0) {
            kill(-jobs[i].pgid, SIGHUP);
            kill(-jobs[i].pgid, SIGCONT);
        }
    }
}

void rapelz_shell_shutdown(void) {
    terminate_jobs();
    if (shell_job_control) {
        tcsetpgrp(STDIN_FILENO, shell_pgid);
        tcsetattr(STDIN_FILENO, TCSADRAIN, &shell_tmodes);
    }
}

static int valid_name(const char *s) {
    if (!s || !*s || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
    for (size_t i = 1; s[i]; i++)
        if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return 0;
    return 1;
}

static int split_assignment(const char *s, char *name, size_t nsize,
                            const char **value) {
    const char *eq = strchr(s, '=');
    if (!eq || eq == s) return 0;
    size_t n = (size_t)(eq - s);
    if (n >= nsize) return 0;
    memcpy(name, s, n);
    name[n] = '\0';
    if (!valid_name(name)) return 0;
    if (value) *value = eq + 1;
    return 1;
}

static int path_matches_current_directory(const char *path) {
    struct stat path_stat, current_stat;
    return path && path[0] == '/' &&
           stat(path, &path_stat) == 0 &&
           stat(".", &current_stat) == 0 &&
           path_stat.st_dev == current_stat.st_dev &&
           path_stat.st_ino == current_stat.st_ino;
}

/* Normalisasi . dan .. secara leksikal tanpa menyelesaikan symlink. */
static int normalize_absolute_path(const char *path, char *out, size_t cap) {
    size_t component_start[RAPELZ_INPUT_SIZE / 2 + 1];
    size_t depth = 0;
    size_t out_len = 1;

    if (!path || path[0] != '/' || !out || cap < 2 ||
        strlen(path) >= cap) return -1;

    out[0] = '/';
    out[1] = '\0';

    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);

        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (depth > 0) out_len = component_start[--depth];
            out[out_len] = '\0';
            continue;
        }

        if (depth >= sizeof(component_start) / sizeof(component_start[0]))
            return -1;

        size_t rollback = out_len;
        if (out_len > 1) {
            if (out_len + 1 >= cap) return -1;
            out[out_len++] = '/';
        }
        if (len >= cap - out_len) return -1;
        memcpy(out + out_len, start, len);
        out_len += len;
        out[out_len] = '\0';
        component_start[depth++] = rollback;
    }

    return 0;
}

static int builtin_cd(char **av, int ac) {
    int arg = 1;
    int physical = 0;

    while (arg < ac) {
        if (!strcmp(av[arg], "-L")) physical = 0;
        else if (!strcmp(av[arg], "-P")) physical = 1;
        else break;
        arg++;
    }
    if (arg < ac && !strcmp(av[arg], "--")) arg++;
    if (ac - arg > 1) {
        fprintf(stderr, "rapelz: cd: terlalu banyak argumen\n");
        return 2;
    }

    const char *target = arg < ac ? av[arg] : getenv("HOME");
    if (!target || !*target) {
        fprintf(stderr, "rapelz: cd: HOME tidak tersedia\n");
        return 1;
    }

    int dash = strcmp(target, "-") == 0;
    if (dash) {
        target = getenv("OLDPWD");
        if (!target || !*target) {
            fprintf(stderr, "rapelz: cd: OLDPWD tidak tersedia\n");
            return 1;
        }
    }

    char old_physical[4096];
    if (!getcwd(old_physical, sizeof(old_physical))) {
        fprintf(stderr, "rapelz: cd: %s\n", strerror(errno));
        return 1;
    }

    const char *environment_pwd = getenv("PWD");
    char old_logical[4096];
    if (path_matches_current_directory(environment_pwd))
        snprintf(old_logical, sizeof(old_logical), "%s", environment_pwd);
    else
        snprintf(old_logical, sizeof(old_logical), "%s", old_physical);

    char requested[4096];
    if (physical) {
        snprintf(requested, sizeof(requested), "%s", target);
    } else if (target[0] == '/') {
        if (snprintf(requested, sizeof(requested), "%s", target) >=
            (int)sizeof(requested)) {
            fprintf(stderr, "rapelz: cd: jalur terlalu panjang\n");
            return 1;
        }
    } else {
        const char *base = path_matches_current_directory(environment_pwd)
            ? environment_pwd : old_physical;
        int n = snprintf(requested, sizeof(requested), "%s/%s", base, target);
        if (n < 0 || (size_t)n >= sizeof(requested)) {
            fprintf(stderr, "rapelz: cd: jalur terlalu panjang\n");
            return 1;
        }
    }

    char logical_target[4096];
    if (!physical) {
        if (normalize_absolute_path(requested, logical_target,
                                    sizeof(logical_target)) != 0) {
            fprintf(stderr, "rapelz: cd: jalur terlalu panjang atau tidak valid\n");
            return 1;
        }
        target = logical_target;
    }

    if (chdir(target) != 0) {
        int e = errno;
        fprintf(stderr, "rapelz: cd: %s: %s\n", target, strerror(e));
        return 1;
    }

    char newpwd[4096];
    if (physical) {
        if (!getcwd(newpwd, sizeof(newpwd))) {
            fprintf(stderr, "rapelz: cd: %s\n", strerror(errno));
            return 1;
        }
    } else if (path_matches_current_directory(logical_target)) {
        snprintf(newpwd, sizeof(newpwd), "%s", logical_target);
    } else if (!getcwd(newpwd, sizeof(newpwd))) {
        fprintf(stderr, "rapelz: cd: %s\n", strerror(errno));
        return 1;
    }

    if (setenv("OLDPWD", old_logical, 1) != 0 ||
        setenv("PWD", newpwd, 1) != 0) {
        fprintf(stderr, "rapelz: cd: gagal memperbarui lingkungan: %s\n",
                strerror(errno));
        return 1;
    }

    if (dash) puts(newpwd);
    return 0;
}

static int builtin_dirs(char **av, int ac) {
    (void)av;
    if (ac != 1) {
        fprintf(stderr, "rapelz: dirs: tidak menerima argumen\n");
        return 2;
    }

    char current[4096];
    if (!getcwd(current, sizeof(current))) {
        fprintf(stderr, "rapelz: dirs: %s\n", strerror(errno));
        return 1;
    }

    printf("%s", current);
    for (size_t i = directory_stack_count; i > 0; i--)
        printf(" %s", directory_stack[i - 1]);
    putchar('\n');
    return 0;
}

static int builtin_pushd(char **av, int ac) {
    if (ac > 2) {
        fprintf(stderr, "rapelz: pushd: penggunaan: pushd [DIREKTORI]\n");
        return 2;
    }

    if (ac == 1 && directory_stack_count == 0) {
        fprintf(stderr, "rapelz: pushd: stack direktori kosong\n");
        return 1;
    }
    if (ac == 2 && directory_stack_count >= RAPELZ_DIR_STACK_MAX) {
        fprintf(stderr, "rapelz: pushd: stack direktori penuh (maksimum %d)\n",
                RAPELZ_DIR_STACK_MAX);
        return 1;
    }

    char current[4096];
    if (!getcwd(current, sizeof(current))) {
        fprintf(stderr, "rapelz: pushd: %s\n", strerror(errno));
        return 1;
    }

    const char *target = ac == 2 ? av[1] : directory_stack[directory_stack_count - 1];
    char *cd_argv[] = { "cd", (char *)target, NULL };
    int status = builtin_cd(cd_argv, 2);
    if (status != 0) return status;

    if (ac == 2) {
        snprintf(directory_stack[directory_stack_count],
                 sizeof(directory_stack[directory_stack_count]), "%s", current);
        directory_stack_count++;
    } else {
        snprintf(directory_stack[directory_stack_count - 1],
                 sizeof(directory_stack[directory_stack_count - 1]), "%s", current);
    }

    return builtin_dirs((char *[]){ "dirs", NULL }, 1);
}

static int builtin_popd(char **av, int ac) {
    (void)av;
    if (ac != 1) {
        fprintf(stderr, "rapelz: popd: tidak menerima argumen\n");
        return 2;
    }
    if (directory_stack_count == 0) {
        fprintf(stderr, "rapelz: popd: stack direktori kosong\n");
        return 1;
    }

    const char *target = directory_stack[directory_stack_count - 1];
    char *cd_argv[] = { "cd", (char *)target, NULL };
    int status = builtin_cd(cd_argv, 2);
    if (status != 0) return status;

    directory_stack_count--;
    directory_stack[directory_stack_count][0] = '\0';
    return builtin_dirs((char *[]){ "dirs", NULL }, 1);
}

static int builtin_pwd(char **av, int ac) {
    int physical = 0;
    for (int i = 1; i < ac; i++) {
        if (!strcmp(av[i], "-P")) physical = 1;
        else if (!strcmp(av[i], "-L")) physical = 0;
        else {
            fprintf(stderr, "rapelz: pwd: opsi tidak dikenal: %s\n", av[i]);
            return 2;
        }
    }

    char cwd[4096];
    if (!physical) {
        const char *logical = getenv("PWD");
        if (path_matches_current_directory(logical)) {
            puts(logical);
            return 0;
        }
    }

    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "rapelz: pwd: %s\n", strerror(errno));
        return 1;
    }
    puts(cwd);
    return 0;
}

static int builtin_exit(char **av, int ac, int in_child) {
    if (ac > 2) {
        fprintf(stderr, "rapelz: exit: terlalu banyak argumen\n");
        return 2;
    }
    int st = rapelz_last_status();
    if (ac == 2) {
        char *end = NULL;
        long v = strtol(av[1], &end, 10);
        if (!*av[1] || *end || v < 0 || v > 255) {
            fprintf(stderr, "rapelz: exit: argumen harus angka 0-255\n");
            return 2;
        }
        st = (int)v;
    }
    if (in_child) _exit(st);
    exit(st);
}

static int builtin_export(char **av, int ac) {
    if (ac == 1) {
        extern char **environ;
        for (char **e = environ; e && *e; e++) puts(*e);
        return 0;
    }

    for (int i = 1; i < ac; i++) {
        char name[256];
        const char *value;
        if (!split_assignment(av[i], name, sizeof(name), &value)) {
            /*
             * RAPELZ menyimpan variabel shell langsung di environment. Karena
             * itu, export NAME yang sudah valid cukup menjadi no-op; nilai
             * variabel yang ada tidak boleh dihapus atau diubah.
             */
            if (valid_name(av[i])) continue;
            fprintf(stderr, "rapelz: export: format variabel tidak valid: %s\n", av[i]);
            return 2;
        }
        if (setenv(name, value, 1) != 0) {
            fprintf(stderr, "rapelz: export: %s\n", strerror(errno));
            return 1;
        }
    }
    return 0;
}

static int builtin_unset(char **av, int ac) {
    for (int i = 1; i < ac; i++) {
        if (!valid_name(av[i])) {
            fprintf(stderr, "rapelz: unset: nama tidak valid: %s\n", av[i]);
            return 2;
        }
        unsetenv(av[i]);
    }
    return 0;
}

static int builtin_history(char **av, int ac) {
    if (ac == 2 && !strcmp(av[1], "-c")) {
        return rapelz_history_clear() == 0 ? 0 : 1;
    }
    if (ac > 1) {
        fprintf(stderr, "rapelz: history: opsi tidak dikenal\n");
        return 2;
    }
    rapelz_history_print();
    return 0;
}

static int builtin_alias(char **av, int ac) {
    if (ac == 1) {
        rapelz_alias_print();
        return 0;
    }

    int st = 0;
    for (int i = 1; i < ac; i++) {
        char *eq = strchr(av[i], '=');
        if (!eq) {
            const char *v = rapelz_alias_get(av[i]);
            if (v) printf("alias %s='%s'\n", av[i], v);
            else { fprintf(stderr, "rapelz: alias: tidak ditemukan: %s\n", av[i]); st = 1; }
            continue;
        }
        *eq = '\0';
        if (rapelz_alias_set(av[i], eq + 1) != 0) {
            fprintf(stderr, "rapelz: alias: nama atau nilai tidak valid\n");
            st = 2;
        }
        *eq = '=';
    }
    return st;
}

static int builtin_unalias(char **av, int ac) {
    if (ac < 2) {
        fprintf(stderr, "rapelz: unalias: butuh nama alias\n");
        return 2;
    }
    int st = 0;
    for (int i = 1; i < ac; i++)
        if (rapelz_alias_unset(av[i]) != 0) { fprintf(stderr, "rapelz: unalias: tidak ditemukan: %s\n", av[i]); st = 1; }
    return st;
}

static int builtin_set(char **av, int ac) {
    if (ac == 1) {
        printf("pipefail=%s\n", pipefail_option ? "on" : "off");
        return 0;
    }
    if (ac == 3 && !strcmp(av[1], "-o") && !strcmp(av[2], "pipefail")) {
        pipefail_option = 1;
        return 0;
    }
    if (ac == 3 && !strcmp(av[1], "+o") && !strcmp(av[2], "pipefail")) {
        pipefail_option = 0;
        return 0;
    }
    fprintf(stderr, "rapelz: set: dukungannya hanya -o/+o pipefail\n");
    return 2;
}

static int lookup_command(const char *name, char *out, size_t cap) {
    if (!name || !*name) return 0;
    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0) {
            snprintf(out, cap, "%s", name);
            return 1;
        }
        return 0;
    }

    const char *path = getenv("PATH");
    if (!path) return 0;

    const char *p = path;
    while (1) {
        const char *colon = strchr(p, ':');
        size_t n = colon ? (size_t)(colon - p) : strlen(p);
        char dir[4096];
        if (n == 0) snprintf(dir, sizeof(dir), ".");
        else if (n < sizeof(dir)) { memcpy(dir, p, n); dir[n] = '\0'; }
        else { if (!colon) break; p = colon + 1; continue; }

        char full[4096];
        int m = snprintf(full, sizeof(full), "%s/%s", dir, name);
        if (m > 0 && (size_t)m < sizeof(full)) {
            struct stat st;
            if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && access(full, X_OK) == 0) {
                snprintf(out, cap, "%s", full);
                return 1;
            }
        }
        if (!colon) break;
        p = colon + 1;
    }
    return 0;
}

int rapelz_builtin_exists(const char *name) {
    static const char *const b[] = {
        "alias","bg","cd","command","eval","exit","false","fg","help","history",
        "jobs","pwd","set","source","true","type","which","unalias","unset","wait","export",".",
        "dirs","pushd","popd"
    };
    for (size_t i = 0; i < sizeof(b)/sizeof(b[0]); i++)
        if (!strcmp(name, b[i])) return 1;
    return 0;
}

static int builtin_type(char **av, int ac) {
    int all = ac > 1 && !strcmp(av[1], "-a");
    int start = all ? 2 : 1;
    int st = 0;
    if (start >= ac) {
        fprintf(stderr, "rapelz: type: penggunaan: type [-a] NAME...\n");
        return 2;
    }
    for (int i = start; i < ac; i++) {
        int found = 0;
        if (rapelz_alias_exists(av[i])) {
            printf("%s adalah alias\n", av[i]);
            found = 1;
            if (!all) continue;
        }
        if (rapelz_builtin_exists(av[i])) {
            printf("%s adalah builtin RAPELZ\n", av[i]);
            found = 1;
            if (!all) continue;
        }
        char p[4096];
        if (lookup_command(av[i], p, sizeof(p))) {
            printf("%s -> %s\n", av[i], p);
            found = 1;
        }
        if (!found) {
            printf("%s: tidak ditemukan\n", av[i]);
            st = 1;
        }
    }
    return st;
}

static int builtin_command(char **av, int ac) {
    if (ac < 2) return 0;
    if (!strcmp(av[1], "-v") || !strcmp(av[1], "-V")) {
        int verbose = !strcmp(av[1], "-V");
        if (ac < 3) return 0;
        int st = 0;
        for (int i = 2; i < ac; i++) {
            char p[4096];
            if (rapelz_alias_exists(av[i])) {
                if (verbose) printf("%s adalah alias\n", av[i]);
                else puts(av[i]);
            } else if (rapelz_builtin_exists(av[i])) {
                if (verbose) printf("%s adalah builtin RAPELZ\n", av[i]);
                else puts(av[i]);
            } else if (lookup_command(av[i], p, sizeof(p))) {
                if (verbose) printf("%s\n", p);
                else puts(p);
            } else {
                if (verbose) printf("%s: tidak ditemukan\n", av[i]);
                st = 1;
            }
        }
        return st;
    }

    int start = 1;
    if (!strcmp(av[start], "-p")) {
        if (ac < 3) return 0;
        start++;
    }
    if (start >= ac) return 0;

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "rapelz: command: %s\n", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        restore_child_signals();
        execvp(av[start], av + start);
        fprintf(stderr, "rapelz: %s: %s\n", av[start], strerror(errno));
        _exit(errno == ENOENT ? 127 : 126);
    }

    int st = 1;
    if (status_of_pid(pid, &st) != 0) return 1;
    return wait_status(st);
}

static int builtin_which(char **av, int ac) {
    if (ac < 2) {
        fprintf(stderr, "rapelz: which: penggunaan: which NAME...\n");
        return 2;
    }
    int st = 0;
    for (int i = 1; i < ac; i++) {
        char path[4096];
        if (lookup_command(av[i], path, sizeof(path))) puts(path);
        else {
            fprintf(stderr, "rapelz: which: %s: tidak ditemukan\n", av[i]);
            st = 1;
        }
    }
    return st;
}

static int builtin_help(void) {
    puts("RAPELZ builtin:");
    puts("cd pwd pushd popd dirs exit export unset history alias unalias set source . eval type type -a command -v/-V which");
    puts("jobs fg bg wait true false help");
    puts("Operator: | |& < > >> 2> 2>> n>&m n>&- && || ; &");
    puts("Shortcut editor: Ctrl+A/E/B/F/P/N/R/L/C/D/W/U/K/T/_");
    return 0;
}

static int builtin_eval(char **av, int ac) {
    char line[RAPELZ_INPUT_SIZE] = {0};
    size_t n = 0;
    for (int i = 1; i < ac; i++) {
        size_t l = strlen(av[i]);
        if (n + l + 2 >= sizeof(line)) return 2;
        if (i > 1) line[n++] = ' ';
        memcpy(line + n, av[i], l); n += l;
    }
    line[n] = '\0';
    return rapelz_run_line(line);
}

static int builtin_source(char **av, int ac) {
    if (ac != 2) {
        fprintf(stderr, "rapelz: source: penggunaan: source FILE\n");
        return 2;
    }
    return rapelz_execute_script_file(av[1]);
}

static int builtin_simple(char **av, int ac, int in_child) {
    if (ac == 0) return 0;
    if (!strcmp(av[0], "cd")) return builtin_cd(av, ac);
    if (!strcmp(av[0], "pushd")) return builtin_pushd(av, ac);
    if (!strcmp(av[0], "popd")) return builtin_popd(av, ac);
    if (!strcmp(av[0], "dirs")) return builtin_dirs(av, ac);
    if (!strcmp(av[0], "pwd")) return builtin_pwd(av, ac);
    if (!strcmp(av[0], "exit")) return builtin_exit(av, ac, in_child);
    if (!strcmp(av[0], "export")) return builtin_export(av, ac);
    if (!strcmp(av[0], "unset")) return builtin_unset(av, ac);
    if (!strcmp(av[0], "history")) return builtin_history(av, ac);
    if (!strcmp(av[0], "alias")) return builtin_alias(av, ac);
    if (!strcmp(av[0], "unalias")) return builtin_unalias(av, ac);
    if (!strcmp(av[0], "set")) return builtin_set(av, ac);
    if (!strcmp(av[0], "type")) return builtin_type(av, ac);
    if (!strcmp(av[0], "which")) return builtin_which(av, ac);
    if (!strcmp(av[0], "help")) return builtin_help();
    if (!strcmp(av[0], "eval")) return builtin_eval(av, ac);
    if (!strcmp(av[0], "source") || !strcmp(av[0], ".")) return builtin_source(av, ac);
    if (!strcmp(av[0], "command")) return builtin_command(av, ac);
    if (!strcmp(av[0], "true")) return 0;
    if (!strcmp(av[0], "false")) return 1;
    return -1;
}

static int job_slot(void) {
    for (int i = 0; i < RAPELZ_MAX_JOBS; i++)
        if (!jobs[i].used) return i;
    return -1;
}

static job_t *find_job_id(int id) {
    for (int i = 0; i < RAPELZ_MAX_JOBS; i++)
        if (jobs[i].used && jobs[i].id == id) return &jobs[i];
    return NULL;
}

/*
 * 0 berarti spesifikasi kosong (pilih job bawaan), ID positif valid,
 * dan -1 berarti format tidak valid. atoi() tidak aman di sini karena
 * teks seperti "abc" diam-diam berubah menjadi ID 0.
 */
static int parse_job_spec(const char *s) {
    if (!s || !*s) return 0;
    if (*s == '%') s++;
    if (!*s) return -1;

    int id = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!isdigit(*p)) return -1;
        int digit = (int)(*p - '0');
        if (id > (9999 - digit) / 10) return -1;
        id = id * 10 + digit;
    }
    return id > 0 ? id : -1;
}

static void remove_job(job_t *j) {
    if (j) memset(j, 0, sizeof(*j));
}

static int add_job(pid_t pgid, const pid_t *pids, int count,
                   const char *command, int state, int status) {
    int i = job_slot();

    /* Job selesai yang belum di-wait boleh dipertahankan, tetapi jangan
     * sampai memenuhi seluruh tabel jika pengguna tidak pernah memanggil wait. */
    if (i < 0) {
        for (int j = 0; j < RAPELZ_MAX_JOBS; j++) {
            if (jobs[j].used && jobs[j].state == 3) {
                remove_job(&jobs[j]);
                i = j;
                break;
            }
        }
    }
    if (i < 0) {
        fprintf(stderr, "rapelz: daftar job penuh\n");
        return -1;
    }

    memset(&jobs[i], 0, sizeof(jobs[i]));
    jobs[i].used = 1;
    jobs[i].id = next_job_id++;
    if (next_job_id > 9999) next_job_id = 1;
    jobs[i].pgid = pgid;
    jobs[i].count = count;
    jobs[i].state = state;
    jobs[i].status = status;
    jobs[i].pipefail = pipefail_option;
    memcpy(jobs[i].pids, pids, (size_t)count * sizeof(pid_t));
    for (int k = 0; k < count; k++) jobs[i].statuses[k] = -1;
    snprintf(jobs[i].command, sizeof(jobs[i].command), "%s", command ? command : "");
    if (shell_interactive) fprintf(stdout, "[%d] %d\n", jobs[i].id, (int)pgid);
    return jobs[i].id;
}

void rapelz_jobs_reap(void) {
    int st;
    for (;;) {
        pid_t pid = waitpid_nointr(-1, &st, WNOHANG | WUNTRACED | WCONTINUED);
        if (pid <= 0) break;

        for (int j = 0; j < RAPELZ_MAX_JOBS; j++) {
            if (!jobs[j].used) continue;
            for (int k = 0; k < jobs[j].count; k++) {
                if (jobs[j].pids[k] != pid) continue;
                if (WIFSTOPPED(st)) {
                    jobs[j].state = 2;
                } else if (WIFCONTINUED(st)) {
                    jobs[j].state = 1;
                } else if (WIFEXITED(st) || WIFSIGNALED(st)) {
                    jobs[j].pids[k] = -1;
                    jobs[j].statuses[k] = wait_status(st);
                }
            }

            int alive = 0;
            for (int k = 0; k < jobs[j].count; k++)
                if (jobs[j].pids[k] > 0) alive = 1;

            if (!alive && jobs[j].used && jobs[j].state != 2 && jobs[j].state != 3) {
                jobs[j].status = pipeline_status(
                    jobs[j].statuses, jobs[j].count, jobs[j].pipefail);
                jobs[j].state = 3;
                if (shell_interactive) {
                    printf("[%d] selesai %d  %s\n", jobs[j].id, jobs[j].status, jobs[j].command);
                    fflush(stdout);
                }
            }
        }
    }
}

static int apply_redir(const rapelz_redirection_t *r) {
    if (!r) return -1;

    if (r->type == RAPELZ_REDIR_DUP_OUT || r->type == RAPELZ_REDIR_DUP_IN) {
        if (r->dup_fd < 0) return close(r->fd) == 0 ? 0 : -1;
        return dup2(r->dup_fd, r->fd) >= 0 ? 0 : -1;
    }
    if (r->type == RAPELZ_REDIR_CLOSE) return close(r->fd) == 0 ? 0 : -1;
    if (!r->target) return -1;

    int flags = 0;
    mode_t mode = 0666;
    int fd;
    if (r->type == RAPELZ_REDIR_IN) fd = open(r->target, O_RDONLY);
    else {
        flags = O_WRONLY | O_CREAT | (r->type == RAPELZ_REDIR_APPEND ? O_APPEND : O_TRUNC);
        fd = open(r->target, flags, mode);
    }
    if (fd < 0) {
        fprintf(stderr, "rapelz: redirection %s: %s\n", r->target, strerror(errno));
        return -1;
    }
    if (dup2(fd, r->fd) < 0) {
        int e = errno; close(fd); errno = e;
        return -1;
    }

    if (r->dup_fd == -2) {
        if (dup2(fd, STDERR_FILENO) < 0) { int e = errno; close(fd); errno = e; return -1; }
    }
    if (fd != r->fd && fd != STDERR_FILENO) close(fd);
    return 0;
}

static int apply_all_redirs(const rapelz_command_t *cmd) {
    for (int i = 0; i < cmd->redir_count; i++) {
        if (apply_redir(&cmd->redirs[i]) != 0) return -1;
    }
    return 0;
}

static int apply_assignments(const rapelz_command_t *cmd, int persist, int *consumed) {
    int n = 0;
    for (; n < cmd->argc; n++) {
        char name[256];
        const char *value;
        if (!split_assignment(cmd->argv[n], name, sizeof(name), &value)) break;
        if (persist) {
            if (setenv(name, value, 1) != 0) return -1;
        }
    }
    if (consumed) *consumed = n;
    return 0;
}

static int execute_parent_builtin(rapelz_command_t *cmd) {
    int fds[64];
    int saved[64];
    int count = 0;
    int start_fd = 10;

    for (int fd = 0; fd < 3; fd++) fds[count++] = fd;
    for (int i = 0; i < cmd->redir_count; i++) {
        int fd = cmd->redirs[i].fd;
        int exists = 0;
        for (int k = 0; k < count; k++) if (fds[k] == fd) exists = 1;
        if (!exists && count < (int)(sizeof(fds) / sizeof(fds[0]))) fds[count++] = fd;
        if (fd + 1 > start_fd) start_fd = fd + 1;
        if (cmd->redirs[i].dup_fd == -2) {
            exists = 0;
            for (int k = 0; k < count; k++) if (fds[k] == STDERR_FILENO) exists = 1;
            if (!exists && count < (int)(sizeof(fds) / sizeof(fds[0]))) fds[count++] = STDERR_FILENO;
        }
    }

    for (int i = 0; i < count; i++) {
        saved[i] = fcntl(fds[i], F_DUPFD_CLOEXEC, start_fd);
        if (saved[i] < 0) {
            if (errno == EBADF) {
                /* FD was closed before the builtin. Preserve that state. */
                saved[i] = -2;
                continue;
            }
            for (int k = 0; k < i; k++) if (saved[k] >= 0) close(saved[k]);
            return 1;
        }
        if (saved[i] >= start_fd) start_fd = saved[i] + 1;
    }

    fflush(NULL);
    int st;
    if (apply_all_redirs(cmd) != 0) st = 1;
    else st = builtin_simple(cmd->argv, cmd->argc, 0);
    fflush(NULL);

    for (int i = 0; i < count; i++) {
        if (saved[i] == -2) {
            if (close(fds[i]) != 0 && errno != EBADF) st = 1;
        } else {
            if (dup2(saved[i], fds[i]) < 0) st = 1;
            close(saved[i]);
        }
    }
    return st;
}

static int run_child_builtin_or_exec(rapelz_command_t *cmd) {
    int consumed = 0;
    if (apply_assignments(cmd, 1, &consumed) != 0) _exit(1);

    /*
     * Redirection tetap harus diproses untuk perintah yang hanya berisi
     * assignment, misalnya: VAR=nilai >berkas.
     */
    if (apply_all_redirs(cmd) != 0) _exit(1);

    if (consumed == cmd->argc) {
        _exit(0);
    }

    rapelz_command_t tmp = *cmd;
    int n = cmd->argc - consumed;
    for (int i = 0; i < n; i++) tmp.argv[i] = cmd->argv[consumed + i];
    for (int i = n; i < RAPELZ_MAX_ARGS; i++) tmp.argv[i] = NULL;
    tmp.argc = n;

    int jb = run_job_builtin(&tmp);
    if (jb >= 0) _exit(jb & 255);

    int st = builtin_simple(tmp.argv, tmp.argc, 1);
    if (st >= 0) _exit(st & 255);

    execvp(tmp.argv[0], tmp.argv);
    fprintf(stderr, "rapelz: %s: %s\n", cmd->argv[consumed], strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
}

static int status_of_pid(pid_t pid, int *st_out) {
    int st;
    pid_t result = waitpid_nointr(pid, &st, WUNTRACED);
    if (result != pid) return -1;
    if (st_out) *st_out = st;
    return 0;
}

static void pipeline_text(const rapelz_pipeline_t *pl, char *out, size_t cap);

static int execute_pipeline(rapelz_pipeline_t *pl, int background) {
    int n = pl->command_count;
    int pipes[RAPELZ_MAX_PIPE_COMMANDS - 1][2];
    memset(pipes, -1, sizeof(pipes));

    for (int i = 0; i < n - 1; i++) {
        if (pipe(pipes[i]) != 0) {
            int saved_errno = errno;
            for (int k = 0; k < i; k++) {
                close(pipes[k][0]);
                close(pipes[k][1]);
            }
            fprintf(stderr, "rapelz: pipe: %s\n", strerror(saved_errno));
            errno = saved_errno;
            return 1;
        }
    }

    pid_t pids[RAPELZ_MAX_PIPE_COMMANDS];
    int statuses[RAPELZ_MAX_PIPE_COMMANDS];
    memset(pids, 0, sizeof(pids));
    memset(statuses, 0, sizeof(statuses));
    pid_t pgid = 0;

    for (int i = 0; i < n; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            int e = errno;
            fprintf(stderr, "rapelz: fork: %s\n", strerror(e));
            for (int k = 0; k < n - 1; k++) { close(pipes[k][0]); close(pipes[k][1]); }
            if (pgid > 0) {
                kill(-pgid, SIGTERM);
                for (int k = 0; k < i; k++)
                    (void)waitpid_nointr(pids[k], NULL, 0);
            }
            errno = e;
            return 1;
        }

        if (pid == 0) {
            if (!pgid) setpgid(0, 0);
            else setpgid(0, pgid);
            restore_child_signals();

            if (i > 0 && dup2(pipes[i - 1][0], STDIN_FILENO) < 0) _exit(125);
            if (i < n - 1) {
                if (dup2(pipes[i][1], STDOUT_FILENO) < 0) _exit(125);
                if (pl->commands[i].pipe_stderr && dup2(pipes[i][1], STDERR_FILENO) < 0) _exit(125);
            }

            for (int k = 0; k < n - 1; k++) {
                close(pipes[k][0]); close(pipes[k][1]);
            }
            run_child_builtin_or_exec(&pl->commands[i]);
            _exit(127);
        }

        if (!pgid) pgid = pid;
        setpgid(pid, pgid);
        pids[i] = pid;
    }

    for (int k = 0; k < n - 1; k++) { close(pipes[k][0]); close(pipes[k][1]); }

    if (background) {
        char jobcmd[RAPELZ_INPUT_SIZE];
        pipeline_text(pl, jobcmd, sizeof(jobcmd));
        if (add_job(pgid, pids, n, jobcmd, 1, 0) < 0) {
            kill(-pgid, SIGHUP);
            for (int i = 0; i < n; i++)
                (void)waitpid_nointr(pids[i], NULL, 0);
            return 1;
        }
        return 0;
    }

    if (shell_job_control) tcsetpgrp(STDIN_FILENO, pgid);

    int stopped = 0;
    for (int i = 0; i < n; i++) {
        int st = 0;
        if (status_of_pid(pids[i], &st) != 0) st = 1;
        statuses[i] = wait_status(st);
        if (WIFSTOPPED(st)) stopped = 1;
    }

    if (shell_job_control) tcsetpgrp(STDIN_FILENO, shell_pgid);

    if (stopped) {
        char jobcmd[RAPELZ_INPUT_SIZE];
        pipeline_text(pl, jobcmd, sizeof(jobcmd));
        add_job(pgid, pids, n, jobcmd, 2, statuses[n - 1]);
        return statuses[n - 1];
    }

    if (pipefail_option) {
        for (int i = n - 1; i >= 0; i--)
            if (statuses[i] != 0) return statuses[i];
    }
    return statuses[n - 1];
}

static void pipeline_text(const rapelz_pipeline_t *pl, char *out, size_t cap) {
    size_t n = 0;
    out[0] = '\0';
    for (int i = 0; i < pl->command_count; i++) {
        if (i) {
            const char *op = pl->commands[i - 1].pipe_stderr ? " |& " : " | ";
            size_t l = strlen(op);
            if (n + l + 1 >= cap) break;
            memcpy(out + n, op, l); n += l;
        }
        for (int a = 0; a < pl->commands[i].argc; a++) {
            if (a) out[n++] = ' ';
            size_t l = strlen(pl->commands[i].argv[a]);
            if (n + l + 1 >= cap) break;
            memcpy(out + n, pl->commands[i].argv[a], l); n += l;
            out[n] = '\0';
        }
    }
    out[n] = '\0';
}

static int jobs_builtin(char **av, int ac) {
    (void)av;
    if (ac > 1) return 2;
    rapelz_jobs_reap();
    for (int i = 0; i < RAPELZ_MAX_JOBS; i++) if (jobs[i].used) {
        const char *state = jobs[i].state == 2 ? "berhenti" :
                            jobs[i].state == 3 ? "selesai" : "berjalan";
        printf("[%d] %s  %s\n", jobs[i].id, state, jobs[i].command);
    }
    return 0;
}

static int wait_one_job(job_t *j) {
    if (!j) return 1;

    /* Status selesai dipertahankan sampai wait mengambilnya. */
    if (j->state == 3) {
        int result = j->status;
        remove_job(j);
        return result;
    }

    /* Jangan menunggu proses yang berhenti selamanya. */
    if (j->state == 2) return j->status;

    int result = j->status;
    for (int i = 0; i < j->count; i++) {
        if (j->pids[i] <= 0) continue;

        int st = 0;
        pid_t waited = waitpid_nointr(j->pids[i], &st, WUNTRACED);

        if (waited == j->pids[i]) {
            if (WIFSTOPPED(st)) {
                j->state = 2;
                j->status = wait_status(st);
                return j->status;
            }

            j->pids[i] = -1;
            j->statuses[i] = wait_status(st);
            result = j->statuses[i];
            continue;
        }

        if (waited < 0 && errno == ECHILD) {
            /* Proses mungkin sudah dipanen oleh rapelz_jobs_reap(). */
            j->pids[i] = -1;
            continue;
        }

        if (waited < 0) {
            fprintf(stderr, "rapelz: wait: %s\n", strerror(errno));
            return 1;
        }
    }

    /* Opsi pipefail diambil saat job dibuat, bukan saat wait dipanggil. */
    result = pipeline_status(j->statuses, j->count, j->pipefail);
    remove_job(j);
    return result;
}

static int fg_builtin(char **av, int ac) {
    if (ac > 2) return 2;
    int id = ac == 2 ? parse_job_spec(av[1]) : 0;
    if (id < 0) {
        fprintf(stderr, "rapelz: fg: spesifikasi job tidak valid: %s\n", av[1]);
        return 2;
    }
    if (!id) {
        for (int i = RAPELZ_MAX_JOBS - 1; i >= 0; i--) if (jobs[i].used) { id = jobs[i].id; break; }
    }
    job_t *j = find_job_id(id);
    if (!j) { fprintf(stderr, "rapelz: fg: job tidak ditemukan\n"); return 1; }

    if (j->state == 3) {
        fprintf(stderr, "rapelz: fg: job sudah selesai; gunakan wait untuk mengambil statusnya\n");
        return 1;
    }
    if (shell_job_control) tcsetpgrp(STDIN_FILENO, j->pgid);
    if (kill(-j->pgid, SIGCONT) != 0 && errno != ESRCH) {
        int e = errno;
        if (shell_job_control) tcsetpgrp(STDIN_FILENO, shell_pgid);
        fprintf(stderr, "rapelz: fg: %s\n", strerror(e));
        return 1;
    }
    j->state = 1;

    int result = 0;
    for (int i = 0; i < j->count; i++) if (j->pids[i] > 0) {
        int st = 0;
        pid_t waited = waitpid_nointr(j->pids[i], &st, WUNTRACED);

        if (waited == j->pids[i]) {
            if (WIFSTOPPED(st)) {
                j->state = 2;
                j->status = wait_status(st);
                if (shell_job_control) tcsetpgrp(STDIN_FILENO, shell_pgid);
                return j->status;
            }
            j->pids[i] = -1;
            j->statuses[i] = wait_status(st);
            result = j->statuses[i];
        } else if (waited < 0 && errno != ECHILD) {
            int e = errno;
            if (shell_job_control) tcsetpgrp(STDIN_FILENO, shell_pgid);
            fprintf(stderr, "rapelz: fg: %s\n", strerror(e));
            return 1;
        } else {
            j->pids[i] = -1;
        }
    }
    if (shell_job_control) tcsetpgrp(STDIN_FILENO, shell_pgid);
    /* Gunakan status setiap anggota pipeline, termasuk yang sudah dipanen. */
    result = pipeline_status(j->statuses, j->count, j->pipefail);
    remove_job(j);
    return result;
}

static int bg_builtin(char **av, int ac) {
    if (ac > 2) return 2;
    int id = ac == 2 ? parse_job_spec(av[1]) : 0;
    if (id < 0) {
        fprintf(stderr, "rapelz: bg: spesifikasi job tidak valid: %s\n", av[1]);
        return 2;
    }
    if (!id) {
        for (int i = RAPELZ_MAX_JOBS - 1; i >= 0; i--) if (jobs[i].used) { id = jobs[i].id; break; }
    }
    job_t *j = find_job_id(id);
    if (!j) { fprintf(stderr, "rapelz: bg: job tidak ditemukan\n"); return 1; }
    if (j->state == 3) {
        fprintf(stderr, "rapelz: bg: job sudah selesai\n");
        return 1;
    }
    if (kill(-j->pgid, SIGCONT) != 0) {
        fprintf(stderr, "rapelz: bg: %s\n", strerror(errno));
        return 1;
    }
    j->state = 1;
    return 0;
}

static int wait_builtin(char **av, int ac) {
    int result = 0;
    if (ac == 1) {
        /*
         * wait tanpa operand menunggu semua job yang dikenal. Status exit
         * individual tidak dipakai sebagai status builtin; hasilnya 0.
         */
        for (int i = 0; i < RAPELZ_MAX_JOBS; i++)
            if (jobs[i].used) (void)wait_one_job(&jobs[i]);
        return 0;
    }
    for (int i = 1; i < ac; i++) {
        int id = parse_job_spec(av[i]);
        if (id < 0) {
            fprintf(stderr, "rapelz: wait: spesifikasi job tidak valid: %s\n", av[i]);
            result = 2;
            continue;
        }
        job_t *j = find_job_id(id);
        if (!j) { fprintf(stderr, "rapelz: wait: job tidak ditemukan: %s\n", av[i]); result = 1; continue; }
        result = wait_one_job(j);
    }
    return result;
}


static int is_builtin_name(const char *name) {
    return name && *name && rapelz_builtin_exists(name);
}

static int run_job_builtin(rapelz_command_t *cmd) {
    if (!strcmp(cmd->argv[0], "jobs")) return jobs_builtin(cmd->argv, cmd->argc);
    if (!strcmp(cmd->argv[0], "fg")) return fg_builtin(cmd->argv, cmd->argc);
    if (!strcmp(cmd->argv[0], "bg")) return bg_builtin(cmd->argv, cmd->argc);
    if (!strcmp(cmd->argv[0], "wait")) return wait_builtin(cmd->argv, cmd->argc);
    return -1;
}

static int execute_one_pipeline(rapelz_pipeline_t *pl) {
    for (int i = 0; i < pl->command_count; i++) {
        if (rapelz_expand_command(&pl->commands[i]) != 0) return 1;
    }

    if (pl->command_count == 1 && !pl->background) {
        rapelz_command_t *cmd = &pl->commands[0];
        int consumed = 0;
        if (apply_assignments(cmd, 0, &consumed) != 0) return 1;

        if (consumed == cmd->argc) {
            if (apply_assignments(cmd, 1, NULL) != 0) return 1;
            /*
             * Assignment tanpa command berjalan di shell induk, tetapi
             * redirection-nya tetap sementara dan harus dipulihkan.
             */
            rapelz_command_t assignment_only = *cmd;
            assignment_only.argc = 0;
            assignment_only.argv[0] = NULL;
            return execute_parent_builtin(&assignment_only);
        }

        rapelz_command_t tmp = *cmd;
        const char *name = cmd->argv[consumed];
        int n = cmd->argc - consumed;
        if (consumed > 0) {
            for (int i = 0; i < n; i++) tmp.argv[i] = cmd->argv[consumed + i];
            for (int i = n; i < RAPELZ_MAX_ARGS; i++) tmp.argv[i] = NULL;
            tmp.argc = n;
            name = tmp.argv[0];
        }

        int jb = run_job_builtin(&tmp);
        if (jb >= 0) return jb;

        if (is_builtin_name(name)) {
            /*
             * Prefix assignment untuk builtin diterapkan di shell induk.
             * Untuk command eksternal, biarkan child menerapkannya agar
             * VAR=nilai command tidak bocor ke environment induk.
             */
            if (consumed > 0 && apply_assignments(cmd, 1, NULL) != 0) return 1;
            return execute_parent_builtin(&tmp);
        }
    }

    return execute_pipeline(pl, pl->background);
}

static int execute_program(rapelz_program_t *program) {
    int status = rapelz_last_status();

    for (int i = 0; i < program->pipeline_count; i++) {
        if (i > 0) {
            rapelz_connector_t c = program->connectors[i - 1];
            if (c == RAPELZ_CONN_AND && status != 0) continue;
            if (c == RAPELZ_CONN_OR && status == 0) continue;
        }

        char text[1024];
        pipeline_text(&program->pipelines[i], text, sizeof(text));
        int st = execute_one_pipeline(&program->pipelines[i]);
        if (program->pipelines[i].background && st == 0 && shell_interactive) {
            (void)text;
        }
        status = st;
        rapelz_set_last_status(status);
    }
    return status;
}

int rapelz_run_line(char *line) {
    if (!line) return 2;

    char aliased[RAPELZ_INPUT_SIZE];
    if (rapelz_alias_expand_line(line, aliased, sizeof(aliased)) != 0) {
        rapelz_set_last_status(2);
        return 2;
    }

    rapelz_program_t *p = calloc(1, sizeof(*p));
    if (!p) {
        fprintf(stderr, "rapelz: memori tidak cukup\n");
        rapelz_set_last_status(1);
        return 1;
    }

    if (rapelz_parse_line(aliased, p) != 0) {
        free(p);
        rapelz_set_last_status(2);
        return 2;
    }

    int st = p->pipeline_count ? execute_program(p) : 0;
    rapelz_free_program(p);
    free(p);
    rapelz_set_last_status(st);
    return st;
}

int rapelz_capture_command(const char *line, char **output, size_t *output_len) {
    if (!line || !output || !output_len) return -1;
    *output = NULL; *output_len = 0;

    if (++capture_depth > 8) {
        capture_depth--;
        fprintf(stderr, "rapelz: command substitution terlalu dalam\n");
        return -1;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) { capture_depth--; return -1; }

    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]); capture_depth--; return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(125);
        if (pipefd[1] != STDOUT_FILENO) close(pipefd[1]);
        shell_interactive = 0;
        shell_job_control = 0;
        restore_child_signals();
        char copy[RAPELZ_INPUT_SIZE];
        snprintf(copy, sizeof(copy), "%s", line);
        int st = rapelz_run_line(copy);
        fflush(stdout);
        _exit(st & 255);
    }

    close(pipefd[1]);
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(pipefd[0]);
        kill(pid, SIGTERM);
        (void)waitpid_nointr(pid, NULL, 0);
        capture_depth--;
        return -1;
    }

    while (1) {
        if (len + 2048 + 1 > cap) {
            size_t next = cap * 2;
            char *g = realloc(buf, next);
            if (!g) {
                free(buf); close(pipefd[0]);
                kill(pid, SIGTERM);
                (void)waitpid_nointr(pid, NULL, 0);
                capture_depth--; return -1;
            }
            buf = g; cap = next;
        }
        ssize_t n = read(pipefd[0], buf + len, cap - len - 1);
        if (n > 0) { len += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(pipefd[0]);
    buf[len] = '\0';
    (void)waitpid_nointr(pid, NULL, 0);
    *output = buf;
    *output_len = len;
    capture_depth--;
    return 0;
}

static int read_script_line(FILE *fp, char *line, size_t size) {
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

int rapelz_execute_script_file(const char *path) {
    if (!path || !*path) return 2;
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "rapelz: %s: %s\n", path, strerror(errno));
        return 1;
    }

    char line[RAPELZ_INPUT_SIZE];
    int st = 0;
    int line_result;
    while ((line_result = read_script_line(fp, line, sizeof(line))) != 0) {
        if (line_result < 0) {
            fprintf(stderr, "rapelz: baris script terlalu panjang; diabaikan\n");
            st = 2;
            continue;
        }
        if (!line[0]) continue;
        st = rapelz_run_line(line);
        if (st == 130 || st == 131) break;
    }
    fclose(fp);
    return st;
}
