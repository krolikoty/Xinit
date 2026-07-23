# xinit build system
# FreeBSD: make
# Linux (testing): make

CC      ?= cc
CFLAGS  = -O2 -Wall -Wextra -std=c11
LDFLAGS = -lpthread

PREFIX  ?= /usr/local
DESTDIR ?=

BINDIR  = $(DESTDIR)$(PREFIX)/sbin
MANDIR  = $(DESTDIR)$(PREFIX)/share/man/man8
SVCDIR  = $(DESTDIR)/etc/xinit/services
LOGDIR  = $(DESTDIR)/var/log/xinit
RUNDIR  = $(DESTDIR)/run/xinit

all: xinit xinictl

xinit: src/xinit.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "  LD  $@"

xinictl: src/xinictl.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "  LD  $@"

install: all
	install -d $(BINDIR) $(MANDIR) $(SVCDIR) $(LOGDIR) $(RUNDIR)
	install -m 755 xinit   $(BINDIR)/xinit
	install -m 755 xinictl $(BINDIR)/xinictl
	install -m 644 man/xinit.8   $(MANDIR)/xinit.8   2>/dev/null || true
	install -m 644 man/xinictl.8 $(MANDIR)/xinictl.8 2>/dev/null || true
	@echo ""
	@echo "==> xinit installed to $(BINDIR)"
	@echo "==> Add to /boot/loader.conf:  init_path=\"/sbin/xinit\""
	@echo "==> Service units go in: $(SVCDIR)"

install-examples:
	install -d $(SVCDIR)
	install -m 644 services/*.service $(SVCDIR)/

uninstall:
	rm -f $(BINDIR)/xinit $(BINDIR)/xinictl

clean:
	rm -f xinit xinictl

.PHONY: all install install-examples uninstall clean
