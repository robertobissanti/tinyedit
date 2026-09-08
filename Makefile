CC = cc
CFLAGS = -Wall -O2 -std=c99
TEST_CFLAGS = -std=c99 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wformat=2 -Wundef -Wstrict-prototypes -Wmissing-prototypes

tinyedit: tinyedit.c tinyedit.h clipboard.c clipboard.h utf8.c utf8.h settings.c settings.h backup.c backup.h syntax.c syntax.h
	$(CC) $(CFLAGS) -o tinyedit tinyedit.c clipboard.c utf8.c settings.c backup.c syntax.c

# Installs the shipped syntax configurations into the directory
# tinyedit scans at startup. Deliberately NOT a dependency of the
# `tinyedit` target: building is not consent to write into the user's
# home, and doing it on every build would clobber local edits to these
# files. Existing files are left alone for the same reason -- use
# `make install-syntax-force` to overwrite them with the shipped
# versions.
SYNTAX_DIR = $(HOME)/.tinyedit/syntax

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

clean:
	rm -f tinyedit tests/test_syntax tests/test_settings_backup

tests/test_syntax: tests/test_syntax.c syntax.c syntax.h settings.c settings.h tinyedit.h utf8.c utf8.h
	$(CC) $(TEST_CFLAGS) -I. -o $@ tests/test_syntax.c syntax.c settings.c utf8.c

tests/test_settings_backup: tests/test_settings_backup.c settings.c settings.h backup.c backup.h
	$(CC) $(TEST_CFLAGS) -I. -o $@ tests/test_settings_backup.c settings.c backup.c

test: tinyedit tests/test_syntax tests/test_settings_backup
	./tests/test_syntax
	./tests/test_settings_backup
	python3 tests/test_pty.py

.PHONY: clean test install-syntax install-syntax-force
