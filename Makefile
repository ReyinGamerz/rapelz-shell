CC ?= clang
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
CPPFLAGS ?= -Iinclude

TARGET = rapelz
SRC = src/main.c src/shell.c src/prompt.c src/input.c src/history.c src/completion.c src/suggestion.c src/syntax.c src/parser.c src/alias.c src/config.c

all: $(TARGET)

$(TARGET): $(SRC) include/rapelz.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRC)

clean:
	rm -f $(TARGET) tests/test_completion

tests/test_completion: tests/test_completion.c include/rapelz.h src/completion.c src/alias.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tests/test_completion.c src/completion.c src/alias.c

test: $(TARGET) tests/test_completion
	sh ./tests/test_core.sh
	./tests/test_completion

test-pty: $(TARGET)
	python3 ./tests/test_input_pty.py

.PHONY: all clean test test-pty
