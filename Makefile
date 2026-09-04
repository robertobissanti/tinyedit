CC = cc
CFLAGS = -Wall -O2 -std=c99

tinyedit: tinyedit.c tinyedit.h clipboard.c clipboard.h utf8.c utf8.h settings.c settings.h
	$(CC) $(CFLAGS) -o tinyedit tinyedit.c clipboard.c utf8.c settings.c

clean:
	rm -f tinyedit

.PHONY: clean
