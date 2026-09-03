CC = cc
CFLAGS = -Wall -O2 -std=c99

tinyedit: tinyedit.c
	$(CC) $(CFLAGS) -o tinyedit tinyedit.c

clean:
	rm -f tinyedit

.PHONY: clean
