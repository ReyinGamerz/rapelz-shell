# RAPELZ Shell

RAPELZ adalah shell terminal mandiri yang ditulis dari nol menggunakan C untuk Termux/Linux. RAPELZ tidak memakai Bash atau Zsh sebagai mesin shell.

## v0.8

Rilis ini memperkuat fondasi shell: parser, expansion, pipeline, redirection, operator, environment, alias, history persisten, line editor, autosuggestion, completion, syntax highlighting, dan job control dasar.

## Fitur

Shell core:
- quote tunggal dan ganda
- backslash escape
- ekspansi environment dengan $VAR dan bentuk parameter sederhana
- status terakhir dengan $?
- PID shell dengan $$
- tilde expansion
- command substitution $(...) termasuk nested
- globbing * ? [...]
- komentar dengan #
- pipeline | dan |&
- redirection < > >> 2> 2>>
- dup/close seperti 2>&1 dan 2>&-
- combined output &> dan &>>
- operator ; && ||
- background &
- set -o pipefail dan set +o pipefail
- lazy expansion agar state dan $? mengikuti command yang benar-benar sudah berjalan

Builtin:
cd, pwd, exit, export, unset, history, alias, unalias, set, source, ., eval, type, command, jobs, fg, bg, wait, true, false, help.

Line editor:
- kiri/kanan
- Home/End
- Delete/Backspace
- Ctrl+A/E/B/F
- Ctrl+P/N
- Ctrl+W/U/K
- Ctrl+R reverse history search
- Ctrl+C
- Ctrl+D
- Ctrl+L
- Ctrl+T
- Ctrl+_ undo sederhana
- Alt+B/F dan Ctrl+Left/Right
- Tab, Ctrl+I, Ctrl+Space/NUL untuk completion
- ↑ dan ↓ menavigasi command history; End menerima autosuggestion. Jika tidak ada suggestion, End pindah ke ujung baris

Enter selalu menjalankan buffer asli. Suggestion abu-abu hanya preview sampai diterima.

Completion dasar dibuat context-aware untuk posisi command, alias, builtin, executable PATH, file/direktori, variable environment, export/unset, alias/unalias, dan redirection. Kandidat diurutkan dan common prefix diisi terlebih dahulu.

History persisten:
1. RAPELZ_HISTORY_FILE
2. XDG_STATE_HOME/rapelz/history
3. HOME/.rapelz_history

Config:
1. RAPELZ_CONFIG
2. XDG_CONFIG_HOME/rapelz/config
3. HOME/.config/rapelz/config

Build dari clone baru:

    pkg update
    pkg install git clang make python -y
    git clone https://github.com/ReyinGamerz/rapelz-shell.git
    cd rapelz-shell
    make clean all
    make test
    make test-pty
    ./rapelz

Binary `rapelz` adalah artefak build dan tidak disimpan di repository. Jalankan `make clean all` untuk membangun ulang dari source sesuai arsitektur perangkat.

Validasi:
Suite test mencakup command substitution nested, lazy expansion, pipeline, pipefail, redirection, append, fd dup/close, assignment, export/unset, alias, &&/||, background + wait, $?, non-interaktif, quote error, dan completion dasar.

Sanitizer Linux digunakan selama pengembangan untuk memory safety dan undefined behavior.

## Batasan

RAPELZ belum mencoba mengkloning seluruh bahasa Zsh/Bash. Belum ada seluruh shell programming language seperti if/for/while/case/function, heredoc/here-string, process substitution, arithmetic expansion penuh, brace expansion penuh, array, associative array, parameter modifier lengkap, multiline display-cell editor UTF-8, job-spec lengkap, dan ekosistem completion command-specific sebesar Zsh/Fish.

Arsitektur RAPELZ sengaja mempertahankan engine shell sendiri agar fitur tersebut dapat ditambah bertahap tanpa menjadikan RAPELZ wrapper Bash/Zsh.
