PREFIX ?= /usr/local
CC = cc
CFLAGS = -Wall -O2
LDFLAGS =

# mupdf does not ship a pkg-config file; link its libraries directly.
MUPDF_LIBS = -lmupdf -lmupdf-third -lmupdf-pkcs7 -lmupdf-threads -lm

NCURSES_CFLAGS = $(shell pkg-config --cflags ncursesw)
NCURSES_LIBS   = $(shell pkg-config --libs   ncursesw)

DJVU_CFLAGS = $(shell pkg-config --cflags libdjvulibre)
DJVU_LIBS   = $(shell pkg-config --libs   libdjvulibre)

POPPLER_CFLAGS = $(shell pkg-config --cflags poppler-cpp)
POPPLER_LIBS   = $(shell pkg-config --libs   poppler-cpp)

all: fbpdf

# fbpdf.o and menu.o need ncurses headers
fbpdf.o: fbpdf.c draw.h doc.h menu.h
	$(CC) -c $(CFLAGS) $(NCURSES_CFLAGS) $<

menu.o: menu.c menu.h
	$(CC) -c $(CFLAGS) $(NCURSES_CFLAGS) $<

%.o: %.c doc.h
	$(CC) -c $(CFLAGS) $<

clean:
	-rm -f *.o fbpdf fbdjvu fbpdf2

# PDF support using mupdf
fbpdf: fbpdf.o menu.o mupdf.o draw.o
	$(CC) -o $@ $^ $(LDFLAGS) $(MUPDF_LIBS) $(NCURSES_LIBS)

# DjVu support
djvulibre.o: djvulibre.c doc.h
	$(CC) -c $(CFLAGS) $(DJVU_CFLAGS) $<

fbdjvu: fbpdf.o menu.o djvulibre.o draw.o
	$(CXX) -o $@ $^ $(LDFLAGS) $(DJVU_LIBS) -lm -lpthread $(NCURSES_LIBS)

# PDF support using poppler
poppler.o: poppler.c
	$(CXX) -c $(CFLAGS) $(POPPLER_CFLAGS) $<

fbpdf2: fbpdf.o menu.o poppler.o draw.o
	$(CXX) -o $@ $^ $(LDFLAGS) $(POPPLER_LIBS) $(NCURSES_LIBS)

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 fbpdf $(DESTDIR)$(PREFIX)/bin/fbpdf
	install -d $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 644 fbpdf.1 $(DESTDIR)$(PREFIX)/share/man/man1/fbpdf.1

.PHONY: all clean install
