CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr/local

cBrowse: cBrowse.c html.c html.h
	$(CC) $(CFLAGS) -o cBrowse cBrowse.c html.c -lm

install: cBrowse
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 cBrowse $(DESTDIR)$(PREFIX)/bin/cBrowse

clean:
	rm -f cBrowse
