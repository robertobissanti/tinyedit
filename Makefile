CC = cc
CFLAGS = -Wall -O2 -std=c99

tinyedit: tinyedit.c clipboard.c clipboard.h
	$(CC) $(CFLAGS) -o tinyedit tinyedit.c clipboard.c

clean:
	rm -f tinyedit

.PHONY: clean
