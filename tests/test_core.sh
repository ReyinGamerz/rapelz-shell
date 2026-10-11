#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="$ROOT/rapelz"
TMPDIR=$(printenv TMPDIR 2>/dev/null || echo /tmp)
TMP="$TMPDIR/rapelz-test-$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

run() {
    script=$1
    printf 'TEST: %s\n' "$script" >&2
    input="$script
exit 0
"
    printf '%s' "$input" | RAPELZ_HISTORY_FILE="$TMP/history" "$BIN" \
        >"$TMP/out" 2>"$TMP/err"
}

assert_out() {
    needle=$1
    grep -F -- "$needle" "$TMP/out" >/dev/null 2>&1 || {
        echo "GAGAL: output tidak memuat: $needle" >&2
        cat "$TMP/out" >&2 || true
        cat "$TMP/err" >&2 || true
        exit 1
    }
}

# Baris kosong dan komentar penuh harus menjadi no-op tanpa error.
run '# komentar RAPELZ'
[ ! -s "$TMP/err" ]
run '   '
[ ! -s "$TMP/err" ]
run 'echo akhir-semicolon;'
assert_out akhir-semicolon
run 'echo sebelum-komentar; # komentar setelah command'
assert_out sebelum-komentar
[ ! -s "$TMP/err" ]

run 'echo $(printf sub)'
assert_out sub

run 'echo pre$(printf sub)post'
assert_out presubpost

run 'false; echo $?'
assert_out 1

run 'export DYN=before; export DYN=after; echo $DYN'
assert_out after

run 'false | true
set -o pipefail
false | true
echo $?'
assert_out 1

run 'echo a && echo b || echo c'
assert_out b

# Regresi parser: quote mempertahankan spasi, escape literal, dan operator invalid ditolak.
run 'echo "dua kata"; printf "%s\n" escaped\ value'
assert_out 'dua kata'
assert_out 'escaped value'
run 'echo "unterminated'
! grep -F 'unterminated' "$TMP/out" >/dev/null
grep -F 'quote belum ditutup' "$TMP/err" >/dev/null
run 'echo should-not-run ||| echo invalid'
! grep -F should-not-run "$TMP/out" >/dev/null
grep -F 'sintaksis' "$TMP/err" >/dev/null

# pwd -L menggunakan PWD hanya jika path itu cocok dengan direktori aktif.
run 'mkdir -p '"$TMP"'/pwd-physical; ln -s '"$TMP"'/pwd-physical '"$TMP"'/pwd-logical; cd '"$TMP"'/pwd-logical; pwd -L; pwd -P; cd -; pwd -L'
assert_out "$TMP/pwd-logical"
assert_out "$TMP/pwd-physical"

# -L menormalisasi .. sebelum chdir; -P mengikuti resolusi symlink kernel.
run 'mkdir -p '"$TMP"'/pwd-base/nest '"$TMP"'/pwd-physical; ln -s '"$TMP"'/pwd-physical '"$TMP"'/pwd-base/nest/logical; cd -L '"$TMP"'/pwd-base/nest/logical/..; pwd -L; pwd -P; cd -P '"$TMP"'/pwd-base/nest/logical/..; pwd -L; pwd -P'
assert_out "$TMP/pwd-base/nest"
assert_out "$TMP"


# History rusak tidak boleh terpecah menjadi entri palsu atau membawa escape terminal.
printf '%s\n' 'echo history-valid' > "$TMP/history-seed"
awk 'BEGIN { for (i=0; i<5000; i++) printf "X"; printf "\n" }' >> "$TMP/history-seed"
printf 'bad\033[31mred\nbad\rterminal-spoof\n' >> "$TMP/history-seed"
printf 'history\nexit 0\n' |
  RAPELZ_HISTORY_FILE="$TMP/history-seed" "$BIN" >"$TMP/history-load.out" 2>"$TMP/history-load.err"
grep -F 'echo history-valid' "$TMP/history-load.out" >/dev/null
! grep -F 'XXXXX' "$TMP/history-load.out" >/dev/null
! grep -F "$(printf '\033[31m')" "$TMP/history-load.out" >/dev/null
! grep -F 'terminal-spoof' "$TMP/history-load.out" >/dev/null

# history -c tetap harus menghapus file jika file hanya bisa dibaca.
printf '%s\n' 'echo stale-history' > "$TMP/history-readonly"
chmod 400 "$TMP/history-readonly"
printf 'history -c\nexit 0\n' |
  RAPELZ_HISTORY_FILE="$TMP/history-readonly" "$BIN" >"$TMP/history-clear.out" 2>"$TMP/history-clear.err"
! grep -F 'echo stale-history' "$TMP/history-readonly" >/dev/null

# History yang sudah ada harus selalu dipersempit menjadi mode privat 0600.
printf '%s\n' 'echo old-history' > "$TMP/history-permissions"
chmod 644 "$TMP/history-permissions"
printf 'exit 0\n' |
  RAPELZ_HISTORY_FILE="$TMP/history-permissions" "$BIN" >"$TMP/history-permissions.out" 2>"$TMP/history-permissions.err"
[ "$(stat -c '%a' "$TMP/history-permissions")" = 600 ]

# File history berupa symlink tidak boleh diikuti atau dimodifikasi.
printf '%s\n' 'private-target' > "$TMP/history-target"
ln -s "$TMP/history-target" "$TMP/history-symlink"
printf 'exit 0\n' |
  RAPELZ_HISTORY_FILE="$TMP/history-symlink" "$BIN" >"$TMP/history-symlink.out" 2>"$TMP/history-symlink.err"
[ "$(cat "$TMP/history-target")" = "private-target" ]
[ -L "$TMP/history-symlink" ]



printf '%s\n' 'printf abc > '"$TMP"'/file' \
  'printf def >> '"$TMP"'/file' \
  'cat '"$TMP"'/file' | \
  RAPELZ_HISTORY_FILE="$TMP/history-file-test" "$BIN" >"$TMP/fileout" 2>"$TMP/fileerr"
grep -F abcdef "$TMP/fileout" >/dev/null

run 'printf x >&2 &>> '"$TMP"'/both'
[ "$(cat "$TMP/both")" = x ]

# Descriptor duplikasi harus berupa angka atau '-', bukan nama file terselubung.
(
  cd "$TMP"
  printf 'echo bad 2>&abc\necho bad 2>&1foo\nexit 0\n' |
    RAPELZ_HISTORY_FILE="$TMP/redir-invalid-history" "$BIN" >"$TMP/redir-invalid.out" 2>"$TMP/redir-invalid.err"
)
[ ! -e "$TMP/abc" ]
! grep -F 'bad foo' "$TMP/redir-invalid.out" >/dev/null
grep -F 'descriptor duplikasi redirection tidak valid' "$TMP/redir-invalid.err" >/dev/null


run 'FOO=rapelz
echo "$FOO"'
assert_out rapelz

# Assignment prefix untuk command eksternal hanya berlaku di child.
run 'RAPELZ_TEMP_SCOPE=private printenv RAPELZ_TEMP_SCOPE
echo "parent=$RAPELZ_TEMP_SCOPE"'
assert_out private
assert_out 'parent='

# Assignment tanpa command tetap persisten dan redirection-nya dibuat.
run 'RAPELZ_REDIRECT_ASSIGN=works > '"$TMP"'/assignment-only
echo "$RAPELZ_REDIRECT_ASSIGN"'
assert_out works
[ -f "$TMP/assignment-only" ]

run 'export BAR=rapelz
env | grep "^BAR="
unset BAR
echo "BAR=$BAR"'

# export NAME tanpa assignment menerima nama yang sudah ada tanpa mengubah nilainya.
run 'export RAPELZ_EXPORT_TEST=works; export RAPELZ_EXPORT_TEST; printenv RAPELZ_EXPORT_TEST'
assert_out works

run 'alias ll="echo alias-ok"
ll; ll | cat'
assert_out alias-ok

run 'sleep 0.02 &
sleep 0.05
wait %1
echo $?'
assert_out 0

# wait tanpa operand menunggu job, tetapi tidak mewarisi status exit child.
run 'false & wait; echo $?'
assert_out 0


# wait dengan ID job harus mempertahankan status nonzero meski child selesai cepat.
run 'false & wait %1; echo $?'
assert_out 1

# Pipeline background juga mempertahankan status pipefail ketika ditunggu setelah selesai.
run 'set -o pipefail; false | true & sleep 0.05; wait %1; echo $?'
assert_out 1


run 'false | true &
sleep 0.05
wait %1
echo $?'
assert_out 0

run 'set -o pipefail
false | true &
sleep 0.05
wait %1
echo $?'
assert_out 1

# fg harus mempertahankan status anggota pipeline yang gagal ketika pipefail aktif.
run 'set -o pipefail; false | sleep 0.05 & fg %1; echo $?'
assert_out 1


# Spesifikasi job harus berupa ID angka, opsional diawali persen.
run 'wait e
echo $?'
assert_out 2
grep -F 'wait: spesifikasi job tidak valid: e' "$TMP/err" >/dev/null

run 'wait %abc
echo $?'
assert_out 2
grep -F 'wait: spesifikasi job tidak valid: %abc' "$TMP/err" >/dev/null

run 'wait %0
echo $?'
assert_out 2
grep -F 'wait: spesifikasi job tidak valid: %0' "$TMP/err" >/dev/null

run 'touch '"$TMP"'/marker; echo '"$TMP"'/m*'
assert_out marker

# Metakarakter glob yang dikutip tetap literal meskipun token juga punya wildcard aktif.
run 'touch '"$TMP"'/glob-a '"$TMP"'/glob-b '"$TMP"'/glob-a'"'"'*'"'"'; echo '"$TMP"'/glob-*'"'"'*'"'"''
assert_out "$TMP/glob-a*"

# Wildcard dari parameter expansion yang tidak dikutip ikut menjalani globbing.
run 'export RAPELZ_GLOB="'$TMP'/glob-*"; echo $RAPELZ_GLOB'
assert_out "$TMP/glob-a"
assert_out "$TMP/glob-a*"
assert_out "$TMP/glob-b"

# Backslash terakhir tidak boleh hilang lalu mengeksekusi command yang berubah.
printf 'echo trailing\\\nexit 0\n' |
  RAPELZ_HISTORY_FILE="$TMP/history-trailing-backslash" "$BIN" >"$TMP/trailing-backslash.out" 2>"$TMP/trailing-backslash.err"
! grep -F 'trailing' "$TMP/trailing-backslash.out" >/dev/null
grep -F 'backslash tanpa karakter lanjutan' "$TMP/trailing-backslash.err" >/dev/null

run 'echo "unterminated'
grep -F 'quote belum ditutup' "$TMP/err" >/dev/null

printf '%s\n' false 'echo $?' | \
  RAPELZ_HISTORY_FILE="$TMP/noninteractive-history" "$BIN" >"$TMP/plain" 2>"$TMP/plain.err"
! grep -q 'RAPELZ Shell\|╭─\|╰─❯' "$TMP/plain"
grep -Fx 1 "$TMP/plain" >/dev/null

# Baris input non-interaktif yang terlalu panjang tidak boleh dieksekusi parsial.
awk 'BEGIN { printf "touch %s", "'$TMP'/baris-panjang"; for (i=0; i<5000; i++) printf " "; printf "\nexit 0\n" }' |
  RAPELZ_HISTORY_FILE="$TMP/long-history" "$BIN" >"$TMP/long.out" 2>"$TMP/long.err"
[ ! -e "$TMP/baris-panjang" ]
grep -F 'baris input terlalu panjang' "$TMP/long.err" >/dev/null

# Script juga harus membuang baris terlalu panjang, bukan mengeksekusi potongannya.
awk 'BEGIN { printf "touch %s", "'$TMP'/script-panjang"; for (i=0; i<5000; i++) printf " "; printf "\n" }' >"$TMP/long-script"
printf 'echo script-lanjut\n' >>"$TMP/long-script"
printf 'source %s\nexit 0\n' "$TMP/long-script" |
  RAPELZ_HISTORY_FILE="$TMP/script-history" "$BIN" >"$TMP/script.out" 2>"$TMP/script.err"
[ ! -e "$TMP/script-panjang" ]
grep -F 'baris script terlalu panjang' "$TMP/script.err" >/dev/null
grep -F 'script-lanjut' "$TMP/script.out" >/dev/null

# Config juga harus mengabaikan baris yang terlalu panjang.
awk 'BEGIN { printf "touch %s", "'$TMP'/config-panjang"; for (i=0; i<5000; i++) printf " "; printf "\n" }' >"$TMP/long-config"
printf 'echo config-lanjut > %s\n' "$TMP/config-lanjut" >>"$TMP/long-config"
printf 'exit 0\n' |
  RAPELZ_CONFIG="$TMP/long-config" RAPELZ_HISTORY_FILE="$TMP/config-history" "$BIN" >"$TMP/config.out" 2>"$TMP/config.err"
[ ! -e "$TMP/config-panjang" ]
[ -f "$TMP/config-lanjut" ]
grep -F 'baris config terlalu panjang' "$TMP/config.err" >/dev/null

# Directory stack: pushd menyimpan direktori asal, popd memulihkannya,
# dan pushd tanpa argumen menukar direktori aktif dengan entri teratas.
run 'mkdir -p '"$TMP"'/dir-stack; pushd '"$TMP"'/dir-stack; pwd; popd; pwd; pushd '"$TMP"'/dir-stack; pushd; pwd; dirs; popd; pwd'
assert_out "$TMP/dir-stack"
assert_out "$ROOT"

# Perintah stack harus tersedia sebagai builtin.
run 'type pushd; type popd; type dirs'
assert_out 'pushd adalah builtin RAPELZ'
assert_out 'popd adalah builtin RAPELZ'
assert_out 'dirs adalah builtin RAPELZ'

# Command discovery compatibility: command -v/-V, type -a, and which.
run 'command -v echo sh; command -V echo; type -a echo; which sh'
assert_out 'echo'
assert_out 'echo adalah builtin RAPELZ'
assert_out '->'
run 'command -v rapelz-command-that-does-not-exist; echo $?'
assert_out 1

echo "OK: test inti RAPELZ lulus"
