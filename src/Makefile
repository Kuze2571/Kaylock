CC = gcc
WAYLAND_PROTOCOLS=/usr/share/wayland-protocols
XDG_SHELL_XML=$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml

CFLAGS = -Wall -Wextra -g $(shell pkg-config --cflags wayland-client) \
    $(shell pkg-config --cflags xkbcommon) \
    $(shell pkg-config --cflags pango) \
    $(shell pkg-config --cflags pangocairo) \
    $(shell pkg-config --cflags cairo)

LDFLAGS = $(shell pkg-config --libs wayland-client) \
    $(shell pkg-config --libs xkbcommon) \
    $(shell pkg-config --libs pango) \
    $(shell pkg-config --libs pangocairo) \
    $(shell pkg-config --libs cairo) \
    -lpam -lm

PROGRAM = kaylock

all: $(PROGRAM)

$(PROGRAM): kaylock.o xdg-shell-protocol.o
	$(CC) -o $@ $^ $(LDFLAGS)

kaylock.o: kaylock.c xdg-shell-client-protocol.h
	$(CC) $(CFLAGS) -c -o $@ $<

xdg-shell-protocol.o: xdg-shell-protocol.c
	$(CC) $(CFLAGS) -c -o $@ $<

xdg-shell-protocol.c:
	wayland-scanner private-code $(XDG_SHELL_XML) xdg-shell-protocol.c

xdg-shell-client-protocol.h:
	wayland-scanner client-header $(XDG_SHELL_XML) xdg-shell-client-protocol.h

clean:
	rm -f $(PROGRAM) *.o xdg-shell-protocol.c xdg-shell-client-protocol.h

install: $(PROGRAM)
	install -D -m 755 $(PROGRAM) /usr/local/bin/$(PROGRAM)
	@echo "auth       sufficient pam_unix.so nullok try_first_pass" > kaylock
	@echo "auth       required   pam_deny.so" >> kaylock
	@echo "account    required    pam_unix.so" >> kaylock
	@echo "password   required    pam_permit.so" >> kaylock
	@echo "session    required    pam_unix.so" >> kaylock
	install -D -m 644 kaylock /etc/pam.d/kaylock

.PHONY: all clean install
