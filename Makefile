PREFIX ?= /usr/local
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra
PKGS = wayland-client wayland-egl egl glesv2
PROTO = protocol/wlr-layer-shell-unstable-v1.xml
XDG = $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml
GEN = wlr-layer-shell-unstable-v1-client-protocol.h wlr-layer-shell-unstable-v1-protocol.c xdg-shell-protocol.c

lava: lava.c $(GEN)
	$(CC) $(CFLAGS) $(LDFLAGS) -I. -o $@ lava.c wlr-layer-shell-unstable-v1-protocol.c xdg-shell-protocol.c \
		$$(pkg-config --cflags --libs $(PKGS))

wlr-layer-shell-unstable-v1-client-protocol.h: $(PROTO)
	wayland-scanner client-header $< $@
wlr-layer-shell-unstable-v1-protocol.c: $(PROTO)
	wayland-scanner private-code $< $@
xdg-shell-protocol.c: $(XDG)
	wayland-scanner private-code $< $@

install: lava
	install -Dm755 lava $(DESTDIR)$(PREFIX)/bin/lava

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/lava

clean:
	rm -f lava $(GEN)

.PHONY: install uninstall clean
