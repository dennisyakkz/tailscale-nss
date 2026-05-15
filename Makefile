CC      := gcc
CFLAGS  := -Wall -Wextra -Wpedantic -fPIC -O2 -D_GNU_SOURCE
LDFLAGS := -shared -Wl,-soname,libnss_tailscale.so.2
LIBS    := -lpthread

TARGET  := libnss_tailscale.so.2
OBJ     := nss_tailscale.o

# Detect multiarch library path (e.g. x86_64-linux-gnu)
MULTIARCH := $(shell gcc -print-multiarch 2>/dev/null)
ifeq ($(MULTIARCH),)
    LIBDIR := /usr/lib
else
    LIBDIR := /usr/lib/$(MULTIARCH)
endif

CACHEDIR := /var/cache/tailscale-nss

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $< $(LIBS)

$(OBJ): nss_tailscale.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -D -m 755 $(TARGET) $(DESTDIR)$(LIBDIR)/$(TARGET)
	install -d -m 1777 $(DESTDIR)$(CACHEDIR)
	@echo ""
	@echo "Installation complete."
	@echo "Add 'tailscale' to the hosts line in /etc/nsswitch.conf, e.g.:"
	@echo "  hosts: files tailscale dns"

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(TARGET)
	rm -rf $(DESTDIR)$(CACHEDIR)

clean:
	rm -f $(OBJ) $(TARGET)
