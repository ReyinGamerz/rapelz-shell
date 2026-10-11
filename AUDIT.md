# Audit teknis RAPELZ Shell

> **Pembaruan 2026-10-11:** pemeriksaan dan tes lokal dilakukan pada source dari ZIP v0.8, bukan pada checkout GitHub yang saat pemeriksaan hanya berisi `LICENSE`. Temuan lama di bawah adalah daftar risiko historis untuk diverifikasi ulang, bukan semuanya bug aktif yang sudah direproduksi.

## Status validasi terbaru

- `make clean all` dengan GCC dan Clang serta flag `-Wconversion -Wshadow -Werror`: lulus pada Linux.
- `make test`: lulus, termasuk regresi quote/escape, sintaks operator invalid, `wait %ID`, pipeline background, dan `pipefail`.
- `make test-pty`: lulus pada Linux, termasuk penyisipan karakter UTF-8 dan penghapusan karakter multibyte saat cursor dipindah.
- Build dan tes inti dengan AddressSanitizer/UBSan: lulus pada source kerja saat audit ini; suite PTY dijalankan terpisah pada build biasa.
- **Belum tervalidasi:** runtime Termux/Android, perilaku keyboard Android fisik/virtual, resize terminal, serta rilis lintas ABI.

## Perubahan audit pada source kerja

1. Menambahkan `.gitignore` untuk binary dan artefak build; binary tidak boleh dikomit sebagai source distribusi.
2. Editor menggerakkan cursor dan menghapus berdasarkan batas UTF-8, memasukkan rangkaian UTF-8 bersama, dan memperkirakan lebar kolom untuk karakter kombinasi, CJK, serta emoji.
3. Menambah regresi shell-level untuk quote, escape, operator invalid, `wait %ID`, dan status pipeline background dengan `pipefail`.
4. Menambah regresi PTY untuk menghapus karakter UTF-8 di tengah baris.

Lebar kolom Unicode masih berupa pendekatan, bukan implementasi penuh Unicode grapheme cluster atau `wcwidth()`. Escape sequence keyboard yang tidak valid dan variasi terminal Android tetap perlu diuji langsung.

## Temuan yang harus ditutup sebelum menganggap fondasi siap

### P1 — Job control dan status proses

1. `rapelz_jobs_reap()` menghapus record job segera setelah semua proses selesai. Jika proses background selesai dan dipanen sebelum perintah `wait %ID` dijalankan, status job dapat hilang sehingga `wait` melaporkan job tidak ditemukan. Tambahkan status selesai yang tetap bisa dikonsumsi `wait`, lalu tes dengan jeda di antara background job dan `wait`.
2. Pemilihan status pipeline di `rapelz_jobs_reap()` menggunakan kondisi `k == last || jobs[j].status == 0`. Ini dapat membuat status akhir bergantung pada urutan proses yang dipanen; definisikan dengan jelas status pipeline (termasuk opsi pipefail) dan uji pipeline background yang gagal.
3. `fg_builtin()` menunggu tiap PID dengan satu panggilan `waitpid` tanpa retry EINTR dan belum mengelola semua hasil wait/error secara seragam. Samakan penanganan wait dengan jalur foreground lain, termasuk proses berhenti dan proses yang sudah dipanen.

### P1 — Editor terminal

1. `read_escape()` membaca sisa escape sequence secara blocking tanpa timeout. Tombol Escape tunggal atau terminal yang mengirim sequence terpotong dapat membuat editor menunggu input berikutnya.
2. Panjang/cursor editor dihitung dalam byte. Gerakan cursor dan penghapusan dapat memotong karakter UTF-8 multibyte; ini perlu ditetapkan sebagai batasan atau ditangani dengan editor berbasis karakter tampilan.
3. Tes CI tidak mensimulasikan TTY/PTY. Tambahkan tes pseudo-terminal untuk panah atas/bawah, End, Home, Delete, Ctrl+C, EOF, dan escape sequence yang terpotong. Tetap lakukan tes manual di Termux Android karena keyboard bisa mengirim sequence berbeda.

### P1 — Parser dan expansion

1. Expansion kata saat ini menghapus quote dan mengembangkan nilai ke satu buffer string, sehingga belum mempertahankan metadata quote per karakter. Perilaku seperti wildcard yang berasal dari variable expansion dapat berbeda dari shell POSIX.
2. `parse_redirection()` mengubah descriptor dengan `strtol`, tetapi perlu tes untuk descriptor ekstrem, redirection yang berurutan, dan urutan redirection yang memengaruhi hasil.
3. Batas kapasitas (argumen, pipeline, redirection, panjang baris) perlu punya tes batas tepat di bawah, pada, dan di atas kapasitas agar kegagalan parsial tidak terjadi.

### P2 — History, config, dan alias

1. History/config disimpan sebagai baris teks dan belum memiliki strategi untuk data yang mengandung newline atau kontrol selain penyaringan suggestion. Pastikan input kontrol tidak dapat merusak tampilan terminal.
2. Penulisan history memakai append dan mode file 0600, tetapi belum menguji dua instance shell yang menulis file history yang sama bersamaan.
3. Ekspansi alias dibatasi 64 putaran dan punya penanganan overflow. Tambahkan tes untuk alias berputar, alias di posisi argumen, dan alias sebelum redirection/operator.

### P2 — UX dan rilis

1. README harus selalu sama dengan pemetaan tombol dan perilaku aktual.
2. Tes warna/prompt tidak menggantikan tes TTY. Pastikan redraw, resize terminal, prompt panjang, dan direktori kerja yang dihapus tidak membuat editor rusak.
3. CI Linux/sanitizer tidak membuktikan build atau perilaku Android/Termux. Tambahkan checklist validasi Termux dan prosedur instalasi/pemulihan.
4. Belum ada target rilis yang memverifikasi versi, changelog, dan kompatibilitas sebelum tag.

## Checklist penutupan

- [ ] Job selesai tetap dapat ditunggu dan status pipeline konsisten.
- [ ] Regression test PTY untuk input interaktif lulus.
- [ ] Test parser/expansion dan kapasitas batas lulus.
- [ ] GCC, Clang, ASan, dan UBSan lulus.
- [ ] Build dan uji manual di Termux Android lulus.
- [ ] README, help, dan dokumentasi batasan sinkron.
- [ ] Rilis dapat diinstal dan dipulihkan secara terukur.

## Aturan kerja

Setiap perbaikan wajib menyertakan tes regresi, tidak mengubah perilaku yang tidak terkait, dan menunggu seluruh CI lulus sebelum dianggap selesai. Jangan mengklaim kompatibilitas Bash/Zsh untuk fitur yang belum diuji.
