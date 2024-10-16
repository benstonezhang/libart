CC=gcc
LD=gcc
PREFIX=/usr/local
LIBDIR=$(PREFIX)/lib
INCLUDEDIR=$(PREFIX)/include
CFLAGS=-g -std=c99 -D_GNU_SOURCE -Wall -Werror -O3
LDFLAGS=-g
SHCFLAGS=$(CFLAGS) -fPIC
SHLINKFLAGS=$(LDFLAGS) -shared

all:	src/libart.so

clean:
	rm -f src/*.o src/libart.* tests/*.o test_runner

src/libart.so:	src/libart.o
	$(LD) $(SHLINKFLAGS) -o $@ $<

src/art.c:	src/art.h

src/libart.o:	src/art.c
	$(CC) $(SHCFLAGS) -o $@ -c $<

install:	src/libart.so
	mkdir -p $(DESTDIR)$(LIBDIR)
	mkdir -p $(DESTDIR)$(INCLUDEDIR)
	cp src/libart.so $(DESTDIR)$(LIBDIR)/libart.so
	chmod 555 $(DESTDIR)$(LIBDIR)/libart.so
	cp src/art.h $(DESTDIR)$(INCLUDEDIR)/art.h
	chmod 444 $(DESTDIR)$(INCLUDEDIR)/art.h

tests/runner.o:	tests/runner.c tests/test_art.c
	$(CC) $(CFLAGS) -Isrc -Ideps/check-0.9.8/src -o $@ -c $<

test_runner:	tests/runner.o src/libart.so
	$(LD) $(LDFLAGS) -o $@ $< -Lsrc -Ldeps/check-0.9.8/src/.libs -lcheck -lart
