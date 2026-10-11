#!/usr/bin/env python3
"""Uji regresi editor terminal tanpa perangkat Android fisik."""
import errno
import os
import pty
import select
import signal
import tempfile
import termios
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "rapelz")


def read_available(fd, timeout):
    ready, _, _ = select.select([fd], [], [], timeout)
    if not ready:
        return b""
    try:
        return os.read(fd, 4096)
    except OSError as exc:
        if exc.errno == errno.EIO:
            return b""
        raise


def main():
    with tempfile.TemporaryDirectory(prefix="rapelz-pty-") as tmp:
        history = os.path.join(tmp, "history")
        with open(history, "w", encoding="utf-8") as history_file:
            history_file.write("echo suggested-from-history\n")
        physical_path = os.path.join(tmp, "prompt-physical")
        logical_path = os.path.join(tmp, "prompt-logical")
        os.mkdir(physical_path)
        os.symlink(physical_path, logical_path)
        pid, master = pty.fork()
        if pid == 0:
            env = os.environ.copy()
            env["RAPELZ_HISTORY_FILE"] = history
            try:
                os.execve(BIN, [BIN], env)
            except OSError as exc:
                os.write(2, ("gagal menjalankan RAPELZ: %s\n" % exc).encode())
                os._exit(127)

        output = bytearray()
        deadline = time.monotonic() + 5.0
        try:
            # Tunggu prompt sekaligus pastikan editor benar-benar masuk mode raw.
            while time.monotonic() < deadline:
                output.extend(read_available(master, 0.05))
                if b"\xe2\x9d\xaf " in output:
                    try:
                        flags = termios.tcgetattr(master)[3]
                    except OSError:
                        flags = termios.ICANON
                    if not (flags & termios.ICANON):
                        break
            else:
                raise AssertionError("RAPELZ tidak masuk mode input interaktif")

            # Kirim beberapa baris sekaligus untuk mereproduksi input yang antre.
            os.write(
                master,
                b"echo suggested\x1b[F\n\x1b[A\necho first-pty\necho second-pty\ncd "
                + os.fsencode(logical_path)
                + b"\npwd -L\npwd -P\necho caf\xc3\xa9X\x1b[D\x7f\necho utf8-final\nexit\n",
            )
            deadline = time.monotonic() + 6.0
            status = None
            while time.monotonic() < deadline:
                output.extend(read_available(master, 0.05))
                waited, current = os.waitpid(pid, os.WNOHANG)
                if waited == pid:
                    status = current
                    break

            if status is None:
                raise AssertionError("RAPELZ macet setelah menerima beberapa baris input")

            # Baca sisa keluaran terminal setelah child selesai.
            end = time.monotonic() + 0.25
            while time.monotonic() < end:
                chunk = read_available(master, 0.03)
                if not chunk:
                    break
                output.extend(chunk)

            if not os.WIFEXITED(status) or os.WEXITSTATUS(status) != 0:
                raise AssertionError("RAPELZ keluar dengan status tidak normal")
            if b"suggested-from-history" not in output:
                raise AssertionError("tombol End tidak menerima autosuggestion")
            if output.count(b"suggested-from-history") < 2:
                raise AssertionError("panah atas tidak memanggil ulang command history")
            if b"first-pty" not in output or b"second-pty" not in output:
                raise AssertionError("input antre hilang; keluaran kedua command tidak lengkap")
            if b"\x1b[?25hcafX\r\n" not in output:
                raise AssertionError("backspace/cursor memecah karakter UTF-8 saat edit baris")
            if b"\x1b[1;37m" + os.fsencode(logical_path) not in output:
                raise AssertionError("prompt tidak mempertahankan jalur logis dari PWD")

            print("OK: tes pseudo-terminal mempertahankan input beberapa baris")
        finally:
            try:
                os.close(master)
            except OSError:
                pass
            try:
                waited, _ = os.waitpid(pid, os.WNOHANG)
                if waited == 0:
                    os.kill(pid, signal.SIGKILL)
                    os.waitpid(pid, 0)
            except (ChildProcessError, ProcessLookupError):
                pass


if __name__ == "__main__":
    main()
