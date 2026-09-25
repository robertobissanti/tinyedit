CC ?= cc
CFLAGS ?= -Wall -O2 -D_FORTIFY_SOURCE=2 -std=c99
CPPFLAGS ?= -Iinc
TEST_CFLAGS ?= -O2 -D_FORTIFY_SOURCE=2 -std=c99 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wformat=2 -Wundef -Wstrict-prototypes -Wmissing-prototypes

BIN_DIR := bin
TARGET := $(BIN_DIR)/tinyedit
SRC_DIR := src
INC_DIR := inc
TEST_DIR := tests

SOURCES := $(SRC_DIR)/tinyedit.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c \
	$(SRC_DIR)/render.c $(SRC_DIR)/editor_state.c $(SRC_DIR)/clipboard.c \
	$(SRC_DIR)/utf8.c $(SRC_DIR)/settings.c $(SRC_DIR)/backup.c \
	$(SRC_DIR)/syntax.c $(SRC_DIR)/terminal.c $(SRC_DIR)/alloc.c
HEADERS := $(wildcard $(INC_DIR)/*.h)
TEST_BINS := $(TEST_DIR)/test_syntax $(TEST_DIR)/test_settings_backup \
	$(TEST_DIR)/test_editor_state $(TEST_DIR)/test_buffer $(TEST_DIR)/test_history \
	$(TEST_DIR)/test_render

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
INSTALL ?= install
SYNTAX_DIR ?= $(HOME)/.tinyedit/syntax

$(TARGET): $(SOURCES) $(HEADERS) | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

$(BIN_DIR):
	mkdir -p $@

# This is intentionally opt-in: building never modifies the user's shell or
# PATH. Choose a BINDIR already present in PATH, for example /opt/homebrew/bin
# on Apple Silicon Homebrew or /usr/local/bin on many POSIX systems.
install: $(TARGET)
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/tinyedit

# Installs shipped syntax configurations into the directory tinyedit scans at
# startup. Building is not consent to write into the user's home, and existing
# files stay untouched unless the force target is requested.
install-syntax:
	@mkdir -p $(SYNTAX_DIR)
	@for f in syntax-configs/*.conf; do \
		target=$(SYNTAX_DIR)/$$(basename $$f); \
		if [ -e "$$target" ]; then \
			echo "skip  $$target (already exists)"; \
		else \
			cp "$$f" "$$target" && echo "copy  $$target"; \
		fi; \
	done

install-syntax-force:
	@mkdir -p $(SYNTAX_DIR)
	@cp syntax-configs/*.conf $(SYNTAX_DIR)/ && echo "overwrote $(SYNTAX_DIR) with shipped configs"

$(TEST_DIR)/test_syntax: $(TEST_DIR)/test_syntax.c $(SRC_DIR)/syntax.c $(SRC_DIR)/settings.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_syntax.c $(SRC_DIR)/syntax.c $(SRC_DIR)/settings.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c

$(TEST_DIR)/test_settings_backup: $(TEST_DIR)/test_settings_backup.c $(SRC_DIR)/settings.c $(SRC_DIR)/backup.c $(SRC_DIR)/alloc.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_settings_backup.c $(SRC_DIR)/settings.c $(SRC_DIR)/backup.c $(SRC_DIR)/alloc.c

$(TEST_DIR)/test_editor_state: $(TEST_DIR)/test_editor_state.c $(SRC_DIR)/editor_state.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_editor_state.c $(SRC_DIR)/editor_state.c

$(TEST_DIR)/test_buffer: $(TEST_DIR)/test_buffer.c $(SRC_DIR)/buffer.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_buffer.c $(SRC_DIR)/buffer.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c

$(TEST_DIR)/test_history: $(TEST_DIR)/test_history.c $(SRC_DIR)/history.c $(SRC_DIR)/buffer.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_history.c $(SRC_DIR)/history.c $(SRC_DIR)/buffer.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c

$(TEST_DIR)/test_render: $(TEST_DIR)/test_render.c $(SRC_DIR)/render.c $(SRC_DIR)/buffer.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_render.c $(SRC_DIR)/render.c $(SRC_DIR)/buffer.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c

test: $(TARGET) $(TEST_BINS)
	./$(TEST_DIR)/test_syntax
	./$(TEST_DIR)/test_settings_backup
	./$(TEST_DIR)/test_editor_state
	./$(TEST_DIR)/test_buffer
	./$(TEST_DIR)/test_history
	./$(TEST_DIR)/test_render
	python3 $(TEST_DIR)/test_pty.py

clean:
	rm -f $(TARGET) $(TEST_BINS)

.PHONY: clean test install install-syntax install-syntax-force
