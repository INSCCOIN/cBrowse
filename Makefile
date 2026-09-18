CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr/local

cBrowse: cBrowse.c html.c html.h mouse.c mouse.h
	$(CC) $(CFLAGS) -o cBrowse cBrowse.c html.c mouse.c -lm

install: cBrowse
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 cBrowse $(DESTDIR)$(PREFIX)/bin/cBrowse

clean:
	rm -f cBrowse
