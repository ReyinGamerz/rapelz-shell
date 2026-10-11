#define _POSIX_C_SOURCE 200809L
#include "rapelz.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static struct termios saved;
static int raw_active;

static size_t utf8_width(const char *s, size_t len) {
    size_t width = 0;
    for (size_t i = 0; i < len;) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80U) { width++; i++; continue; }
        size_t step = (c >= 0xF0U && c <= 0xF4U) ? 4 :
                      (c >= 0xE0U && c <= 0xEFU) ? 3 :
                      (c >= 0xC2U && c <= 0xDFU) ? 2 : 1;
        if (i + step > len) step = 1;
        int valid = step > 1;
        for (size_t j = 1; valid && j < step; j++)
            if (((unsigned char)s[i + j] & 0xC0U) != 0x80U) valid = 0;
        if (!valid) { width++; i++; continue; }

        unsigned int cp;
        if (step == 2) cp = ((unsigned int)(c & 0x1FU) << 6) |
                            ((unsigned int)((unsigned char)s[i + 1] & 0x3FU));
        else if (step == 3) cp = ((unsigned int)(c & 0x0FU) << 12) |
                                 ((unsigned int)((unsigned char)s[i + 1] & 0x3FU) << 6) |
                                 ((unsigned int)((unsigned char)s[i + 2] & 0x3FU));
        else cp = ((unsigned int)(c & 0x07U) << 18) |
                  ((unsigned int)((unsigned char)s[i + 1] & 0x3FU) << 12) |
                  ((unsigned int)((unsigned char)s[i + 2] & 0x3FU) << 6) |
                  ((unsigned int)((unsigned char)s[i + 3] & 0x3FU));

        /* Approximate terminal cell width for combining, CJK and emoji. */
        if ((cp >= 0x0300U && cp <= 0x036FU) ||
            (cp >= 0x1AB0U && cp <= 0x1AFFU) ||
            (cp >= 0xFE00U && cp <= 0xFE0FU)) {
            /* combining mark / variation selector: zero terminal columns */
        } else if ((cp >= 0x1100U && cp <= 0x115FU) ||
                   (cp >= 0x2E80U && cp <= 0xA4CFU && cp != 0x303FU) ||
                   (cp >= 0xAC00U && cp <= 0xD7A3U) ||
                   (cp >= 0xF900U && cp <= 0xFAFFU) ||
                   (cp >= 0xFE10U && cp <= 0xFE6FU) ||
                   (cp >= 0xFF00U && cp <= 0xFF60U) ||
                   (cp >= 0xFFE0U && cp <= 0xFFE6U) ||
                   (cp >= 0x1F300U && cp <= 0x1FAFFU)) {
            width += 2;
        } else {
            width++;
        }
        i += step;
    }
    return width;
}

static size_t utf8_cursor_width(const char *s, size_t pos);

static void write_best_effort(int fd, const void *buf, size_t len) {
    ssize_t result = write(fd, buf, len);
    (void)result;
}

static void raw_off(void) {
    if (!raw_active) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    raw_active = 0;
    write_best_effort(STDOUT_FILENO, "\033[?25h", 6);
}

void rapelz_input_shutdown(void) { raw_off(); }

static int raw_on(void) {
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved) != 0) return -1;
    struct termios t = saved;
    t.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= (tcflag_t)~OPOST;
    t.c_cflag |= CS8;
    t.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0) return -1;
    raw_active = 1;
    return 0;
}

static void redraw(const char *b, size_t c) {
    static const char *p = "\033[1;35m╰─\033[1;36m❯ \033[0m";
    char seq[64];
    size_t n = strlen(b);
    if (c > n) c = n;
    write_best_effort(STDOUT_FILENO, "\033[?25l\r", 7);
    write_best_effort(STDOUT_FILENO, p, strlen(p));
    rapelz_render_line(b, c);
    write_best_effort(STDOUT_FILENO, "\033[K", 3);
    size_t cursor_col = utf8_cursor_width(b, c);
    size_t end_col = utf8_width(b, n);
    if (end_col > cursor_col) {
        int m = snprintf(seq, sizeof(seq), "\033[%zuD", end_col - cursor_col);
        if (m > 0) write_best_effort(STDOUT_FILENO, seq, (size_t)m);
    } else {
        const char *s = rapelz_suggestion_for(b);
        if (s && strlen(s) > n && strncmp(s, b, n) == 0) {
            int m = snprintf(seq, sizeof(seq), "\033[%zuD", utf8_width(s, strlen(s)) - end_col);
            if (m > 0) write_best_effort(STDOUT_FILENO, seq, (size_t)m);
        }
    }
    write_best_effort(STDOUT_FILENO, "\033[?25h", 6);
}

static void accept_suggestion(char *b, size_t *len, size_t *cur, size_t sz) {
    const char *s = rapelz_suggestion_for(b);
    if (!s || strlen(s) <= *len) return;
    snprintf(b, sz, "%s", s);
    *len = strlen(b);
    *cur = *len;
}

static int read_escape_byte(char *out, int timeout_ms);

/* Indeks cursor tetap berupa byte offset, tetapi selalu berada di batas UTF-8. */
static size_t utf8_prev(const char *s, size_t pos) {
    if (pos == 0) return 0;
    pos--;
    while (pos > 0 && (((unsigned char)s[pos] & 0xC0U) == 0x80U)) pos--;
    return pos;
}

static size_t utf8_next(const char *s, size_t len, size_t pos) {
    if (pos >= len) return len;
    pos++;
    while (pos < len && (((unsigned char)s[pos] & 0xC0U) == 0x80U)) pos++;
    return pos;
}


static size_t utf8_cursor_width(const char *s, size_t pos) {
    return utf8_width(s, pos);
}

static int read_utf8_tail(char *out, size_t cap, size_t *count, unsigned char lead) {
    size_t need = lead >= 0xF0U && lead <= 0xF4U ? 4 :
                  lead >= 0xE0U && lead <= 0xEFU ? 3 :
                  lead >= 0xC2U && lead <= 0xDFU ? 2 : 1;
    if (need == 1 || need > cap) { *count = 1; return 0; }
    out[0] = (char)lead;
    for (size_t i = 1; i < need; i++) {
        char next;
        int rc = read_escape_byte(&next, 80);
        if (rc != 1 || (((unsigned char)next & 0xC0U) != 0x80U)) {
            *count = 1;
            return 0;
        }
        out[i] = next;
    }
    *count = need;
    return 0;
}

static void move_word_left(const char *b, size_t *c) {
    while (*c && isspace((unsigned char)b[*c - 1])) (*c)--;
    while (*c && !isspace((unsigned char)b[*c - 1])) (*c)--;
}

static void move_word_right(const char *b, size_t len, size_t *c) {
    while (*c < len && isspace((unsigned char)b[*c])) (*c)++;
    while (*c < len && !isspace((unsigned char)b[*c])) (*c)++;
}

static void delete_prev_word(char *b, size_t *len, size_t *cur) {
    size_t s = *cur;
    while (s && isspace((unsigned char)b[s - 1])) s--;
    while (s && !isspace((unsigned char)b[s - 1])) s--;
    if (s == *cur) return;
    memmove(b + s, b + *cur, *len - *cur + 1);
    *len -= *cur - s;
    *cur = s;
}

static void delete_to_start(char *b, size_t *len, size_t *cur) {
    if (!*cur) return;
    memmove(b, b + *cur, *len - *cur + 1);
    *len -= *cur;
    *cur = 0;
}

static void delete_to_end(char *b, size_t *len, size_t *cur) {
    if (*cur >= *len) return;
    *len = *cur;
    b[*len] = '\0';
}


static void transpose_chars(char *b, size_t len, size_t *cur) {
    if (len < 2 || *cur == 0) return;
    size_t right_start;
    size_t left_start;
    size_t end;
    if (*cur == len) {
        right_start = utf8_prev(b, len);
        left_start = utf8_prev(b, right_start);
        end = len;
    } else {
        left_start = utf8_prev(b, *cur);
        right_start = *cur;
        end = utf8_next(b, len, right_start);
    }
    size_t left_len = right_start - left_start;
    size_t right_len = end - right_start;
    if (!left_len || !right_len) return;
    char tmp[RAPELZ_INPUT_SIZE];
    if (left_len + right_len >= sizeof(tmp)) return;
    memcpy(tmp, b + left_start, left_len + right_len);
    memcpy(b + left_start, tmp + left_len, right_len);
    memcpy(b + left_start + right_len, tmp, left_len);
    *cur = end;
}

static void remember_kill(char *dst, size_t cap, const char *src, size_t n) {
    if (!dst || cap == 0) return;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}


/*
 * Baca byte escape dengan batas waktu. Escape tunggal atau sequence terminal
 * yang terpotong tidak boleh membuat editor menunggu input berikutnya selamanya.
 */
static int read_escape_byte(char *out, int timeout_ms) {
    struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
    int ready;

    do {
        ready = poll(&pfd, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);

    if (ready <= 0) return ready;
    if (!(pfd.revents & (POLLIN | POLLHUP))) return -1;

    ssize_t n;
    do {
        n = read(STDIN_FILENO, out, 1);
    } while (n < 0 && errno == EINTR);

    return n == 1 ? 1 : -1;
}

static int read_escape(char *seq, size_t cap) {
    if (!seq || cap < 2) return -1;

    size_t n = 0;
    int rc = read_escape_byte(seq, 80);
    if (rc <= 0) return rc;

    n = 1;
    if (seq[0] != '[' && seq[0] != 'O') {
        seq[1] = '\0';
        return 1;
    }

    while (n < cap - 1) {
        rc = read_escape_byte(seq + n, 40);
        if (rc <= 0) {
            seq[n] = '\0';
            return (int)n;
        }
        if (seq[n] == '~' || (seq[n] >= '@' && seq[n] <= 'Z')) {
            n++;
            break;
        }
        n++;
    }

    seq[n] = '\0';
    return (int)n;
}

static void handle_escape(char *b, size_t *len, size_t *cur, size_t sz,
                           char *draft, int *hist_nav) {
    char seq[32];
    int n = read_escape(seq, sizeof(seq));
    if (n <= 0) return;

    if (seq[0] != '[' && seq[0] != 'O') {
        if (seq[0] == 'b' || seq[0] == 'B') move_word_left(b, cur);
        else if (seq[0] == 'f' || seq[0] == 'F') move_word_right(b, *len, cur);
        return;
    }

    char final = seq[n - 1];

    if (final == 'A') {
        /* Panah atas khusus untuk menelusuri command history. */
        if (!*hist_nav) {
            snprintf(draft, sz, "%s", b);
            *hist_nav = 1;
        }
        if (rapelz_history_previous(b, sz)) { *len = strlen(b); *cur = *len; }
        return;
    }

    if (final == 'B') {
        if (*hist_nav) {
            if (rapelz_history_next(b, sz)) {
                *len = strlen(b); *cur = *len;
            } else {
                snprintf(b, sz, "%s", draft);
                *len = strlen(b); *cur = *len;
                *hist_nav = 0;
                rapelz_history_reset_navigation();
            }
        }
        return;
    }

    if (final == 'D') {
        if (strstr(seq, "1;5") || strstr(seq, "1;3")) move_word_left(b, cur);
        else if (*cur) *cur = utf8_prev(b, *cur);
        return;
    }

    if (final == 'C') {
        if (strstr(seq, "1;5") || strstr(seq, "1;3")) move_word_right(b, *len, cur);
        else if (*cur < *len) *cur = utf8_next(b, *len, *cur);
        return;
    }

    if (final == 'H' || !strcmp(seq, "1~") || !strcmp(seq, "7~")) { *cur = 0; return; }

    /*
     * End menerima autosuggestion (ESC[F, ESC[4~, ESC[8~).
     * Jika tidak ada suggestion yang cocok, perilakunya tetap pindah
     * ke ujung baris seperti tombol End biasa.
     */
    if (final == 'F' || !strcmp(seq, "4~") || !strcmp(seq, "8~")) {
        const char *suggestion = rapelz_suggestion_for(b);
        if (suggestion && strlen(suggestion) > *len &&
            strncmp(suggestion, b, *len) == 0) {
            accept_suggestion(b, len, cur, sz);
            *hist_nav = 0;
            rapelz_history_reset_navigation();
        } else {
            *cur = *len;
        }
        return;
    }

    if (strstr(seq, "3~") && *cur < *len) {
        size_t next = utf8_next(b, *len, *cur);
        memmove(b + *cur, b + next, *len - next + 1);
        *len -= next - *cur;
    }
}

static void search_render(const char *query, const char *match) {
    write_best_effort(STDOUT_FILENO, "\r\033[2K", 6);
    printf("\033[1;35m(pencarian riwayat mundur)\033[0m ");
    if (query && *query) printf("[%s]", query);
    if (match) printf("  %s", match);
    fflush(stdout);
}

int rapelz_readline(char *buffer, size_t size) {
    if (!buffer || size < 2) return -1;

    /*
     * Semua buffer bantu editor berukuran RAPELZ_INPUT_SIZE. Batasi ukuran
     * efektif supaya caller dengan buffer lebih besar tidak menyebabkan
     * snprintf(draft, size, ...) menulis melewati buffer lokal.
     */
    if (size > RAPELZ_INPUT_SIZE) size = RAPELZ_INPUT_SIZE;

    if (raw_on() != 0) {
        /* fgets menerima int; tolak ukuran yang tidak dapat direpresentasikan. */
        if (size > (size_t)INT_MAX) return -1;
        if (!fgets(buffer, (int)size, stdin)) return -1;

        size_t used = strlen(buffer);
        if (used > 0 && buffer[used - 1] != '\n' && !feof(stdin)) {
            /*
             * Jangan pernah mengeksekusi potongan pertama dari baris yang
             * lebih panjang daripada buffer. Baris sepanjang kapasitas penuh
             * tetap diterima jika karakter berikutnya langsung newline/EOF.
             */
            int ch = getchar();
            if (ch == '\r') {
                int next = getchar();
                if (next != '\n' && next != EOF) ch = next;
                else ch = '\n';
            }
            if (ch != '\n' && ch != EOF) {
                while (ch != '\n' && ch != EOF) ch = getchar();
                fprintf(stderr, "rapelz: baris input terlalu panjang; diabaikan\n");
                buffer[0] = '\0';
                return 0;
            }
        }

        buffer[strcspn(buffer, "\r\n")] = '\0';
        return 0;
    }

    size_t len = 0, cur = 0;
    char draft[RAPELZ_INPUT_SIZE] = {0};
    char search[RAPELZ_INPUT_SIZE] = {0};
    char origin[RAPELZ_INPUT_SIZE] = {0};
    char undo[32][RAPELZ_INPUT_SIZE] = {{0}};
    int undo_top = 0, hist_nav = 0, search_mode = 0;
    char killbuf[RAPELZ_INPUT_SIZE] = {0};

    buffer[0] = '\0';
    rapelz_history_reset_navigation();

    for (;;) {
        unsigned char ch = 0;
        ssize_t got = read(STDIN_FILENO, &ch, 1);
        if (got == 0) { raw_off(); return len ? 0 : -1; }
        if (got < 0) {
            if (errno == EINTR) continue;
            raw_off();
            return -1;
        }

        if (search_mode) {
            if (ch == 7) {
                snprintf(buffer, size, "%s", origin);
                len = strlen(buffer); cur = len; search_mode = 0;
                redraw(buffer, cur);
                continue;
            }
            if (ch == 18) {
                const char *m = rapelz_history_search_backward(search);
                if (m) { snprintf(buffer, size, "%s", m); len = strlen(buffer); cur = len; }
                search_render(search, m);
                continue;
            }
            if (ch == 127 || ch == 8) {
                size_t q = strlen(search);
                if (q) {
                    search[q - 1] = '\0';
                    const char *m = rapelz_history_search_backward(search);
                    if (m) { snprintf(buffer, size, "%s", m); len = strlen(buffer); cur = len; }
                    else { snprintf(buffer, size, "%s", origin); len = strlen(buffer); cur = len; }
                    search_render(search, m);
                }
                continue;
            }
            if (ch == '\r' || ch == '\n') {
                search_mode = 0;
                redraw(buffer, cur);
                continue;
            }
            if (ch >= 32 && ch != 127) {
                size_t q = strlen(search);
                if (q + 1 < sizeof(search)) { search[q] = (char)ch; search[q + 1] = '\0'; }
                const char *m = rapelz_history_search_backward(search);
                if (m) { snprintf(buffer, size, "%s", m); len = strlen(buffer); cur = len; }
                search_render(search, m);
                continue;
            }
            search_mode = 0;
            redraw(buffer, cur);
        }

        if (ch == '\r' || ch == '\n') {
            static const char *p = "\033[1;35m╰─\033[1;36m❯ \033[0m";
            /*
             * Autosuggestion hanya visual. Bersihkan seluruh baris terlebih
             * dahulu supaya teks abu-abu dari history tidak tertinggal ketika
             * Enter ditekan; kemudian gambar ulang buffer yang benar-benar
             * diketik, tanpa suggestion.
             */
            write_best_effort(STDOUT_FILENO, "\033[?25l\r\033[2K", 11);
            write_best_effort(STDOUT_FILENO, p, strlen(p));
            rapelz_render_line_final(buffer);
            write_best_effort(STDOUT_FILENO, "\r\n", 2);
            raw_off();
            rapelz_history_reset_navigation();
            return 0;
        }

        if (ch == 3) {
            static const char *cancel = "\033[?25l^C\r\n";
            write_best_effort(STDOUT_FILENO, cancel, strlen(cancel));
            raw_off();
            buffer[0] = '\0';
            rapelz_set_last_status(130);
            rapelz_history_reset_navigation();
            return 0;
        }

        if (ch == 4) {
            if (!len) {
                raw_off();
                write_best_effort(STDOUT_FILENO, "\r\n", 2);
                return -1;
            }
            if (cur < len) {
                if (undo_top < 32) snprintf(undo[undo_top++], RAPELZ_INPUT_SIZE, "%s", buffer);
                size_t next = utf8_next(buffer, len, cur);
                memmove(buffer + cur, buffer + next, len - next + 1);
                len -= next - cur;
                rapelz_history_reset_navigation();
                redraw(buffer, cur);
            }
            continue;
        }

        if (ch == 12) {
            write_best_effort(STDOUT_FILENO, "\033[2J\033[H", 7);
            rapelz_print_prompt();
            redraw(buffer, cur);
            continue;
        }

        if (ch == 18) {
            snprintf(origin, size, "%s", buffer);
            search[0] = '\0';
            search_mode = 1;
            search_render(search, rapelz_history_search_backward(search));
            continue;
        }

        if (ch == 9 || ch == 0) {
            int old = (int)len;
            int r = rapelz_complete(buffer, &len, &cur, size);
            if (r == 1 || (r == 2 && (int)len != old)) redraw(buffer, cur);
            else if (r == 2) {
                write_best_effort(STDOUT_FILENO, "\r\n", 2);
                rapelz_completion_print(buffer, cur);
                redraw(buffer, cur);
            }
            continue;
        }

        if (ch == 1) { cur = 0; redraw(buffer, cur); continue; }
        if (ch == 5) { cur = len; redraw(buffer, cur); continue; }
        if (ch == 2) { if (cur) cur = utf8_prev(buffer, cur); redraw(buffer, cur); continue; }
        if (ch == 6) { if (cur < len) cur = utf8_next(buffer, len, cur); redraw(buffer, cur); continue; }

        if (ch == 16) {
            if (!hist_nav) { snprintf(draft, size, "%s", buffer); hist_nav = 1; }
            if (rapelz_history_previous(buffer, size)) { len = strlen(buffer); cur = len; }
            redraw(buffer, cur);
            continue;
        }

        if (ch == 14) {
            if (hist_nav) {
                if (rapelz_history_next(buffer, size)) { len = strlen(buffer); cur = len; }
                else {
                    snprintf(buffer, size, "%s", draft);
                    len = strlen(buffer); cur = len; hist_nav = 0;
                    rapelz_history_reset_navigation();
                }
            }
            redraw(buffer, cur);
            continue;
        }

        if (ch == 23) {
            if (cur) {
                if (undo_top < 32) snprintf(undo[undo_top++], RAPELZ_INPUT_SIZE, "%s", buffer);
                size_t start = cur;
                while (start && isspace((unsigned char)buffer[start - 1])) start--;
                while (start && !isspace((unsigned char)buffer[start - 1])) start--;
                remember_kill(killbuf, sizeof(killbuf), buffer + start, cur - start);
                delete_prev_word(buffer, &len, &cur);
                rapelz_history_reset_navigation();
                redraw(buffer, cur);
            }
            continue;
        }

        if (ch == 21) {
            if (cur) {
                if (undo_top < 32) snprintf(undo[undo_top++], RAPELZ_INPUT_SIZE, "%s", buffer);
                remember_kill(killbuf, sizeof(killbuf), buffer, cur);
                delete_to_start(buffer, &len, &cur);
                rapelz_history_reset_navigation();
                redraw(buffer, cur);
            }
            continue;
        }

        if (ch == 11) {
            if (cur < len) {
                if (undo_top < 32) snprintf(undo[undo_top++], RAPELZ_INPUT_SIZE, "%s", buffer);
                remember_kill(killbuf, sizeof(killbuf), buffer + cur, len - cur);
                delete_to_end(buffer, &len, &cur);
                rapelz_history_reset_navigation();
                redraw(buffer, cur);
            }
            continue;
        }

        if (ch == 31) {
            if (undo_top) {
                snprintf(buffer, size, "%s", undo[--undo_top]);
                len = strlen(buffer);
                cur = len;
                rapelz_history_reset_navigation();
                redraw(buffer, cur);
            }
            continue;
        }

        if (ch == 25) {
            size_t add = strlen(killbuf);
            if (add && len + add < size) {
                if (undo_top < 32) snprintf(undo[undo_top++], RAPELZ_INPUT_SIZE, "%s", buffer);
                memmove(buffer + cur + add, buffer + cur, len - cur + 1);
                memcpy(buffer + cur, killbuf, add);
                cur += add;
                len += add;
                rapelz_history_reset_navigation();
                redraw(buffer, cur);
            }
            continue;
        }

        if (ch == 20) {
            if (len >= 2 && cur > 0) {
                if (undo_top < 32) snprintf(undo[undo_top++], RAPELZ_INPUT_SIZE, "%s", buffer);
                transpose_chars(buffer, len, &cur);
                rapelz_history_reset_navigation();
                redraw(buffer, cur);
            }
            continue;
        }

        if (ch == 127 || ch == 8) {
            if (cur) {
                if (undo_top < 32) snprintf(undo[undo_top++], RAPELZ_INPUT_SIZE, "%s", buffer);
                size_t prev = utf8_prev(buffer, cur);
                memmove(buffer + prev, buffer + cur, len - cur + 1);
                len -= cur - prev; cur = prev;
                rapelz_history_reset_navigation();
                redraw(buffer, cur);
            }
            continue;
        }

        if (ch == 27) {
            handle_escape(buffer, &len, &cur, size, draft, &hist_nav);
            redraw(buffer, cur);
            continue;
        }

        if (ch >= 32 && ch != 127) {
            char encoded[4];
            size_t add = 1;
            encoded[0] = (char)ch;
            if (ch >= 0xC2U) (void)read_utf8_tail(encoded, sizeof(encoded), &add, ch);
            if (len + add < size) {
                if (undo_top < 32) snprintf(undo[undo_top++], RAPELZ_INPUT_SIZE, "%s", buffer);
                memmove(buffer + cur + add, buffer + cur, len - cur + 1);
                memcpy(buffer + cur, encoded, add);
                cur += add;
                len += add;
                hist_nav = 0;
                rapelz_history_reset_navigation();
                redraw(buffer, cur);
            }
        }
    }
}
