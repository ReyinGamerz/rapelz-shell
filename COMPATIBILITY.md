# RAPELZ Shell Compatibility Roadmap

Goal: make RAPELZ a dependable standalone shell for Termux and Discord-bot operations, while progressively approaching common Bash/Zsh behavior. This is a roadmap, not a claim that the features below are already implemented.

## Compatibility rules

- Preserve RAPELZ as a C shell; do not silently delegate the whole shell to Bash/Zsh.
- Every feature must have regression tests before it is described as supported.
- Keep Termux/Android as a first-class target. Linux CI is necessary but does not prove Android runtime compatibility.
- Keep secrets out of command output, debug logs, history examples, and test fixtures.
- Do not merge a compatibility milestone until strict GCC/Clang builds, core tests, PTY tests, and sanitizer tests pass.

## Milestone 1 — Reliability before syntax expansion

- [ ] Fix and regression-test job lifecycle, completed-job retention, `wait`, `fg`, `bg`, signals, and pipeline exit status.
- [ ] Make child process signal handling and terminal foreground ownership predictable.
- [ ] Test interactive Ctrl+C/Ctrl+D, terminal resize, partial escape sequences, and EOF.
- [ ] Test file descriptor redirection order and error paths.
- [ ] Add resource-limit tests for long lines, argument count, pipelines, redirections, and nested command substitution.

## Milestone 2 — Everyday command and environment compatibility

- [ ] Implement/test `command -v`, `command -V`, `type -a`, and a `which` equivalent.
- [ ] Implement/test `read`, `umask`, and useful `printf` behavior where not safely provided by external utilities.
- [ ] Define `PATH`, `PWD`, `OLDPWD`, export, assignment scope, and startup-file behavior.
- [ ] Add shell options and status semantics tests.
- [ ] Keep history/config files private and robust against symlinks and malformed content.

## Milestone 3 — Shell language

- [ ] Build a token/AST layer that retains quoting and source positions.
- [ ] Add multiline parsing and syntax diagnostics.
- [ ] Implement `if/then/elif/else/fi`, `for/in/do/done`, `while`, `until`, `break`, and `continue`.
- [ ] Implement shell functions and positional parameters.
- [ ] Add `case/esac`, `return`, `local` (documented scope), and `trap`.
- [ ] Implement heredoc and here-string safely.
- [ ] Improve parameter expansion, field splitting, globbing, command substitution, and arithmetic expansion with quoting metadata.
- [ ] Add arrays and brace expansion only with explicit compatibility tests.

## Milestone 4 — Interactive experience

- [ ] Harden UTF-8 editing and terminal escape parsing.
- [ ] Add configurable key bindings, prompt configuration, and robust completion.
- [ ] Add completion for common Node/npm/Git workflows without assuming every external command is installed.
- [ ] Add startup config files with documented load order and safe error behavior.
- [ ] Test terminal widths, resize, Unicode, and Android virtual keyboard behavior.

## Milestone 5 — Discord bot operations

- [ ] Add a documented bot-runner workflow using the existing Node/npm commands.
- [ ] Provide safe log redirection and log rotation examples.
- [ ] Provide an optional supervisor script with restart backoff and duplicate-process protection.
- [ ] Document Termux battery/background restrictions; a shell alone cannot guarantee 24/7 uptime.
- [ ] Never print .env contents or bot tokens in diagnostics.

## Required CI gate

Each milestone should pass:
1. GCC and Clang with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror`.
2. Core shell regression suite.
3. PTY/editor regression suite.
4. ASan/UBSan tests on Linux.
5. Manual Termux checklist before release.

## Compatibility promise

RAPELZ is not Bash or Zsh yet. Feature names and syntax must not be advertised as compatible until their observable behavior has tests. Exact 99% compatibility is not a realistic single change; it is a sequence of measurable milestones.
