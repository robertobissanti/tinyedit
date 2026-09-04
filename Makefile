CC = cc
CFLAGS = -Wall -O2 -std=c99
TEST_CFLAGS = -std=c99 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wformat=2 -Wundef -Wstrict-prototypes -Wmissing-prototypes

tinyedit: tinyedit.c tinyedit.h clipboard.c clipboard.h utf8.c utf8.h settings.c settings.h backup.c backup.h syntax.c syntax.h
	$(CC) $(CFLAGS) -o tinyedit tinyedit.c clipboard.c utf8.c settings.c backup.c syntax.c

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

.PHONY: clean test
