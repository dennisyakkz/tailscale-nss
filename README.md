# tailscale-nss

A Linux NSS (Name Service Switch) module that resolves Tailscale machine names
using the `.tail` suffix — without relying on MagicDNS or Tailscale's built-in
DNS configuration.

## Why

Tailscale assigns every machine a stable DNS name through MagicDNS, but those
names are long and awkward:

```
i.e. myhost.bilberry-diphda.ts.net
```

MagicDNS also requires it to be enabled in the Tailscale admin console, rewrites
your system DNS configuration in ways that can conflict with existing setups, and
cannot be customised.

This module takes a different approach: it reads the peer list directly from the
`tailscale` CLI and makes those names available to the system resolver under the
short `.tail` suffix:

```
i.e. myhost.tail  → your hosts ipv4 address 
```

No MagicDNS. No DNS rewrites. No admin console changes. Just add one word to
`/etc/nsswitch.conf`.

## What it does

- Registers as an NSS hosts module (`libnss_tailscale.so.2`) that plugs into the
  standard glibc name-resolution stack.
- On a lookup for `<hostname>.tail`, strips the suffix and searches the cached
  peer list for a match (case-insensitive).
- Only **online** peers are included (based on the `Online` field in
  `tailscale status --json`). Your own machine is always included.
- Returns the peer's first IPv4 Tailscale address.
- Supports forward lookups (`getaddrinfo`, `gethostbyname`) as well as reverse
  lookups (`gethostbyaddr`).
- Lookups for names without the `.tail` suffix are ignored immediately, so the
  module has no impact on normal DNS resolution.

### Caching

Because spawning `tailscale status` on every DNS call would be far too slow, the
module uses a two-layer cache:

| Layer | Location | Lifetime |
|---|---|---|
| In-process | process memory | 24 hours |
| On-disk | `/var/cache/tailscale-nss/peers.cache` | 24 hours |

The on-disk cache is shared across all processes, so `tailscale status --json` is
only run once per 24 hours, regardless of how many applications resolve `.tail`
names. When a lookup misses (peer not found in cache), the cache is refreshed at
most once per 60 seconds.

## Prerequisites

- Linux with **glibc** (tested on Ubuntu 22.04 / 24.04, Debian 12)
- **Tailscale** installed and authenticated (`tailscale status` must work)
- **GCC** and **make** for building

## Installation

### 1. Build

```bash
git clone https://github.com/yourname/tailscale-nss.git
cd tailscale-nss
make
```

### 2. Install the library

```bash
sudo make install
```

This copies `libnss_tailscale.so.2` to the correct multiarch library directory
(e.g. `/usr/lib/x86_64-linux-gnu/`) and creates the cache directory
`/var/cache/tailscale-nss` with sticky-bit permissions so any user can refresh
the cache.

### 3. Enable in nsswitch.conf

Edit `/etc/nsswitch.conf` and add `tailscale` to the `hosts` line:

```
hosts: files tailscale dns
```

Place it **before** `dns` so the module is consulted first, and **after**
`files` so `/etc/hosts` entries still take priority.

### 4. Test

```bash
# Forward lookup
getent hosts myhost.tail

# With ping
ping myhost.tail

# Reverse lookup
getent hosts 100.10.10.10
```

## Uninstall

```bash
sudo make uninstall
```

Then remove `tailscale` from the `hosts` line in `/etc/nsswitch.conf`.

## How it works (technical)

glibc implements `getaddrinfo(3)` and `gethostbyname(3)` by iterating over the
services listed in `nsswitch.conf` and calling the corresponding shared library
for each one. For a service named `tailscale`, glibc loads
`libnss_tailscale.so.2` and calls:

| Function | Used by |
|---|---|
| `_nss_tailscale_gethostbyname4_r` | `getaddrinfo` (modern glibc) |
| `_nss_tailscale_gethostbyname2_r` | `getaddrinfo` (fallback), `gethostbyname2` |
| `_nss_tailscale_gethostbyname_r` | `gethostbyname` |
| `_nss_tailscale_gethostbyaddr_r` | `getnameinfo`, reverse lookups |

On the first call after startup (or after the 24-hour TTL expires), the module
runs:

```bash
tailscale status --json
```

and parses the JSON output to extract `HostName`, `TailscaleIPs`, and `Online`
for each peer. The result is written to `/var/cache/tailscale-nss/peers.cache`
atomically (via a temp file + rename) so concurrent processes never read a
partial file.

Subsequent calls hit only the in-process memory cache — no subprocess, no file
I/O, no latency.

## Limitations

- Only IPv4 Tailscale addresses are returned. IPv6 (`fd7a:…`) addresses are
  currently ignored.
- When a lookup fails because the peer is not in the cache (e.g. a newly online
  machine), the cache is refreshed automatically — at most once every 60 seconds.
  So a new peer becomes resolvable within 60 seconds of the first lookup attempt,
  without any manual intervention.
- A peer that goes **offline** is not removed from the cache until the 24-hour TTL
  expires. To force an immediate refresh (e.g. after a peer disconnects), delete
  the cache file:
  ```bash
  sudo rm /var/cache/tailscale-nss/peers.cache
  ```
- Tailscale must be running and reachable by the user executing the first lookup.
  If `tailscale status` fails, the module returns `UNAVAIL` and resolution falls
  through to the next NSS source (typically DNS).

## License

MIT
