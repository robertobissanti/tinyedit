CC ?= cc
CFLAGS ?= -Wall -O2 -D_FORTIFY_SOURCE=2 -std=c99
CPPFLAGS ?= -Iinc
CPPFLAGS += -Ibin
export BUILD_ID
TEST_CFLAGS ?= -O2 -D_FORTIFY_SOURCE=2 -std=c99 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wformat=2 -Wundef -Wstrict-prototypes -Wmissing-prototypes

BIN_DIR := bin
TARGET := $(BIN_DIR)/tinyedit
SRC_DIR := src
INC_DIR := inc
TEST_DIR := tests

SOURCES := $(SRC_DIR)/tinyedit.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c \
	$(SRC_DIR)/render.c $(SRC_DIR)/editor_state.c $(SRC_DIR)/clipboard.c \
	$(SRC_DIR)/utf8.c $(SRC_DIR)/settings.c $(SRC_DIR)/backup.c \
	$(SRC_DIR)/syntax.c $(SRC_DIR)/terminal.c $(SRC_DIR)/alloc.c \
	$(SRC_DIR)/tree.c $(SRC_DIR)/command.c $(SRC_DIR)/menu.c $(SRC_DIR)/fileio.c $(SRC_DIR)/search.c $(SRC_DIR)/links.c
HEADERS := $(wildcard $(INC_DIR)/*.h) $(BIN_DIR)/build_info.h
TEST_BINS := $(TEST_DIR)/test_syntax $(TEST_DIR)/test_settings_backup \
	$(TEST_DIR)/test_editor_state $(TEST_DIR)/test_buffer $(TEST_DIR)/test_history \
	$(TEST_DIR)/test_render $(TEST_DIR)/test_utf8 $(TEST_DIR)/test_core $(TEST_DIR)/test_fileio $(TEST_DIR)/test_search $(TEST_DIR)/test_backup_paths $(TEST_DIR)/test_memory_contracts $(TEST_DIR)/test_menu

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share/tinyedit
CPPFLAGS += -D'TE_DATADIR="$(DATADIR)"'
INSTALL ?= install
SYNTAX_DIR ?= $(HOME)/.tinyedit/syntax
COLORSCHEME_DIR ?= $(HOME)/.tinyedit/color-scheme
DOCS_DIR ?= $(HOME)/.tinyedit

$(TARGET): FORCE $(BIN_DIR)/build_info.h $(SOURCES) $(HEADERS) | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES)

$(BIN_DIR):
	mkdir -p $@

# This is intentionally opt-in: building never modifies the user's shell or
# PATH. Choose a BINDIR already present in PATH, for example /opt/homebrew/bin
# on Apple Silicon Homebrew or /usr/local/bin on many POSIX systems.
install: install-binary install-system-resources

install-binary: $(TARGET)
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)"
	$(INSTALL) -m 755 $(TARGET) "$(DESTDIR)$(BINDIR)/tinyedit"

# Shared defaults are available to every user; never write to HOME under sudo.
install-system-resources:
	$(MAKE) install-docs DOCS_DIR="$(DESTDIR)$(DATADIR)"
	$(INSTALL) -d "$(DESTDIR)$(DATADIR)/syntax" "$(DESTDIR)$(DATADIR)/color-scheme"
	$(INSTALL) -m 644 syntax-configs/*.conf "$(DESTDIR)$(DATADIR)/syntax/"
	$(INSTALL) -m 644 colorschemes/*.conf "$(DESTDIR)$(DATADIR)/color-scheme/"

# Preserve the relative layout used by the documentation links. These are
# reference copies, separate from the user's active syntax and theme folders.
install-docs:
	@$(INSTALL) -d "$(DOCS_DIR)"
	@$(INSTALL) -m 644 README.md CONTRIBUTING.md IDEAS.md LICENSE LICENSE-THIRD-PARTY "$(DOCS_DIR)/"
	@for dir in docs imgs syntax-configs colorschemes; do \
		$(INSTALL) -d "$(DOCS_DIR)/$$dir" || exit 1; \
		for f in "$$dir"/*; do \
			[ -f "$$f" ] || continue; \
			$(INSTALL) -m 644 "$$f" "$(DOCS_DIR)/$$dir/" || exit 1; \
		done; \
	done
	@$(INSTALL) -d "$(DOCS_DIR)/tests"
	@$(INSTALL) -m 644 tests/README.md "$(DOCS_DIR)/tests/README.md"

# Installs shipped syntax configurations into the directory tinyedit scans at
# startup. Building is not consent to write into the user's home, and existing
# files stay untouched unless the force target is requested.
install-syntax:
	@mkdir -p "$(SYNTAX_DIR)"
	@for f in syntax-configs/*.conf; do \
		target="$(SYNTAX_DIR)/$$(basename "$$f")"; \
		if [ -e "$$target" ]; then \
			echo "skip  $$target (already exists)"; \
		else \
			cp "$$f" "$$target" || exit $$?; \
			echo "copy  $$target"; \
		fi; \
	done

# The editor reads presets from ~/.tinyedit/color-scheme/ (F2 -> Colors). Like
# install-syntax, this is opt-in, never overwrites existing files, and stays out
# of the shared resource installation to preserve personal customizations.
install-colorschemes:
	@mkdir -p "$(COLORSCHEME_DIR)"
	@for f in colorschemes/*.conf; do \
		target="$(COLORSCHEME_DIR)/$$(basename $$f)"; \
		if [ -e "$$target" ]; then \
			echo "skip  $$target (already exists)"; \
		else \
			cp "$$f" "$$target" || exit $$?; \
			echo "copy  $$target"; \
		fi; \
	done

install-colorschemes-force:
	@mkdir -p "$(COLORSCHEME_DIR)"
	@cp colorschemes/*.conf "$(COLORSCHEME_DIR)/" && echo "overwrote $(COLORSCHEME_DIR) with shipped color schemes"

# Everything the editor looks for in the invoking user's home (not the guides:
# those have their own install-docs).
install-resources: install-syntax install-colorschemes

install-syntax-force:
	@mkdir -p "$(SYNTAX_DIR)"
	@cp syntax-configs/*.conf "$(SYNTAX_DIR)/" && echo "overwrote $(SYNTAX_DIR) with shipped configs"

$(TEST_DIR)/test_syntax: $(TEST_DIR)/test_syntax.c $(SRC_DIR)/syntax.c $(SRC_DIR)/settings.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c $(SRC_DIR)/fileio.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_syntax.c $(SRC_DIR)/syntax.c $(SRC_DIR)/settings.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c $(SRC_DIR)/fileio.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c

$(TEST_DIR)/test_settings_backup: $(TEST_DIR)/test_settings_backup.c $(SRC_DIR)/settings.c $(SRC_DIR)/backup.c $(SRC_DIR)/alloc.c $(SRC_DIR)/fileio.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c $(SRC_DIR)/utf8.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_settings_backup.c $(SRC_DIR)/settings.c $(SRC_DIR)/backup.c $(SRC_DIR)/alloc.c $(SRC_DIR)/fileio.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c $(SRC_DIR)/utf8.c

$(TEST_DIR)/test_editor_state: $(TEST_DIR)/test_editor_state.c $(SRC_DIR)/editor_state.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_editor_state.c $(SRC_DIR)/editor_state.c

$(TEST_DIR)/test_buffer: $(TEST_DIR)/test_buffer.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_buffer.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c

$(TEST_DIR)/test_history: $(TEST_DIR)/test_history.c $(SRC_DIR)/history.c $(SRC_DIR)/buffer.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_history.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c

$(TEST_DIR)/test_render: $(TEST_DIR)/test_render.c $(SRC_DIR)/render.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_render.c $(SRC_DIR)/render.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c

$(TEST_DIR)/test_utf8: $(TEST_DIR)/test_utf8.c $(SRC_DIR)/render.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_utf8.c $(SRC_DIR)/render.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c

$(TEST_DIR)/test_fileio: $(TEST_DIR)/test_fileio.c $(SRC_DIR)/fileio.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c $(SRC_DIR)/settings.c $(SRC_DIR)/backup.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_fileio.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c $(SRC_DIR)/settings.c $(SRC_DIR)/backup.c

$(TEST_DIR)/test_backup_paths: $(TEST_DIR)/test_backup_paths.c $(SRC_DIR)/backup.c $(SRC_DIR)/fileio.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_backup_paths.c $(SRC_DIR)/fileio.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c

$(TEST_DIR)/test_search: $(TEST_DIR)/test_search.c $(SRC_DIR)/search.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_search.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c

# The included core intentionally accepts prompt formats and variadic format wrappers.
$(TEST_DIR)/test_core: $(TEST_DIR)/test_core.c $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -Wno-format-nonliteral -o $@ $(TEST_DIR)/test_core.c $(filter-out $(SRC_DIR)/tinyedit.c,$(SOURCES))

$(TEST_DIR)/test_memory_contracts: $(TEST_DIR)/test_memory_contracts.c $(SRC_DIR)/clipboard.c $(SRC_DIR)/alloc.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_memory_contracts.c $(SRC_DIR)/alloc.c

$(TEST_DIR)/test_menu: $(TEST_DIR)/test_menu.c $(SRC_DIR)/menu.c $(SRC_DIR)/command.c $(SRC_DIR)/settings.c $(SRC_DIR)/fileio.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_DIR)/test_menu.c $(SRC_DIR)/command.c $(SRC_DIR)/settings.c $(SRC_DIR)/fileio.c $(SRC_DIR)/buffer.c $(SRC_DIR)/history.c $(SRC_DIR)/utf8.c $(SRC_DIR)/alloc.c

test: $(TARGET) $(TEST_BINS)
	./$(TEST_DIR)/test_syntax
	./$(TEST_DIR)/test_settings_backup
	./$(TEST_DIR)/test_editor_state
	./$(TEST_DIR)/test_buffer
	./$(TEST_DIR)/test_history
	./$(TEST_DIR)/test_render
	./$(TEST_DIR)/test_utf8
	./$(TEST_DIR)/test_core
	./$(TEST_DIR)/test_fileio
	./$(TEST_DIR)/test_search
	./$(TEST_DIR)/test_backup_paths
	./$(TEST_DIR)/test_memory_contracts
	./$(TEST_DIR)/test_menu
	python3 $(TEST_DIR)/test_pty.py
	python3 $(TEST_DIR)/test_build.py
	python3 $(TEST_DIR)/test_docs.py
	python3 $(TEST_DIR)/test_install.py

clean:
	rm -f $(TARGET) $(TEST_BINS) $(TEST_DIR)/benchmark_core

.PHONY: clean test install install-binary install-system-resources install-docs install-syntax install-syntax-force install-colorschemes install-colorschemes-force install-resources

$(TEST_DIR)/benchmark_core: $(TEST_DIR)/benchmark_core.c $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(TEST_CFLAGS) -Wno-format-nonliteral -o $@ $(TEST_DIR)/benchmark_core.c $(filter-out $(SRC_DIR)/tinyedit.c,$(SOURCES))

benchmark: $(TEST_DIR)/benchmark_core
	./$(TEST_DIR)/benchmark_core

.PHONY: benchmark

$(BIN_DIR)/build_info.h: FORCE | $(BIN_DIR)
	sh scripts/build-info.sh $@

FORCE:
.PHONY: FORCE
