CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -fPIC -fvisibility=default
LDFLAGS += -shared
LDLIBS += -lX11 -lXi -ldl -lpthread

PREFIX ?= $(HOME)/.local
LIBDIR ?= $(PREFIX)/lib/unity-cursor-wrap

all: libunity-cursor-wrap.so

libunity-cursor-wrap.so: unity-cursor-wrap.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: libunity-cursor-wrap.so
	install -d $(DESTDIR)$(LIBDIR)
	install -m 0755 libunity-cursor-wrap.so $(DESTDIR)$(LIBDIR)/libunity-cursor-wrap.so

clean:
	rm -f libunity-cursor-wrap.so

.PHONY: all install clean
