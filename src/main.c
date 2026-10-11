#include "rapelz.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    char line[RAPELZ_INPUT_SIZE];

    rapelz_shell_init();
    rapelz_history_init();
    rapelz_alias_init();
    rapelz_config_load();

    atexit(rapelz_input_shutdown);
    atexit(rapelz_alias_shutdown);
    atexit(rapelz_history_shutdown);
    atexit(rapelz_shell_shutdown);

    int interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    if (interactive) {
        puts("RAPELZ Shell v" RAPELZ_VERSION);
        puts("Ketik 'help' untuk bantuan, 'exit' untuk keluar.");
    }

    for (;;) {
        rapelz_jobs_reap();
        if (interactive) rapelz_print_prompt();

        int rc = rapelz_readline(line, sizeof(line));
        if (rc < 0) {
            putchar('\n');
            break;
        }

        if (line[0] != '\0') {
            rapelz_history_add(line);
            (void)rapelz_run_line(line);
        }
    }

    return rapelz_last_status();
}
