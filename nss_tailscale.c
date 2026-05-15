/* nss_tailscale.c
 *
 * NSS module that resolves Tailscale hostnames with the .tail suffix.
 * Install as libnss_tailscale.so.2 and add "tailscale" to the hosts
 * line in /etc/nsswitch.conf.
 *
 * Example: hosts: files tailscale dns
 * Resolves: elitebook.tail -> 100.66.68.66
 *
 * Cache lives in CACHE_FILE and is valid for CACHE_TTL seconds.
 * On a stale or empty cache the full peer list is fetched via
 * "tailscale status --json".  Only ONLINE peers are cached.
 */
#include <nss.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <ctype.h>

#define CACHE_DIR          "/var/cache/tailscale-nss"
#define CACHE_FILE         CACHE_DIR "/peers.cache"
#define CACHE_TTL          (24 * 3600)    /* 24 hours */
#define MIN_REFRESH_SEC    60             /* min seconds between forced refreshes */
#define MAX_PEERS          1024
#define HOSTNAME_LEN       256
#define IP_LEN             46             /* max IPv4/IPv6 string */
#define TAIL_SUFFIX        ".tail"
#define TAIL_SUFFIX_LEN    5
#define JSON_BUF_SIZE      (2 * 1024 * 1024)

/* Bounded string copy that always null-terminates dst. */
#define SCOPY(dst, src) do {                        \
    size_t _l = strlen(src);                        \
    size_t _m = sizeof(dst) - 1;                    \
    if (_l > _m) _l = _m;                          \
    memcpy((dst), (src), _l);                       \
    (dst)[_l] = '\0';                               \
} while (0)

/* ------------------------------------------------------------------ */
/* Data types                                                           */
/* ------------------------------------------------------------------ */

struct ts_peer {
    char hostname[HOSTNAME_LEN];
    char ipv4[IP_LEN];
};

/* ------------------------------------------------------------------ */
/* In-process cache (protected by g_mutex)                             */
/* ------------------------------------------------------------------ */

static struct ts_peer   g_peers[MAX_PEERS];
static int              g_count         = 0;
static time_t           g_loaded        = 0;
static time_t           g_last_refresh  = 0;
static pthread_mutex_t  g_mutex         = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/* Minimal JSON helpers                                                 */
/* ------------------------------------------------------------------ */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/* Parse a JSON string into out[0..outlen-1].
 * Returns pointer past the closing '"', or NULL on error. */
static const char *parse_str(const char *p, char *out, size_t outlen)
{
    if (!p || *p != '"') return NULL;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            if (!*p) return NULL;
            char c = *p;
            if      (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            if (i + 1 < outlen) out[i++] = c;
        } else {
            if (i + 1 < outlen) out[i++] = *p;
        }
        p++;
    }
    if (*p != '"') return NULL;
    out[i] = '\0';
    return p + 1;
}

static const char *skip_str(const char *p)
{
    if (!p || *p != '"') return NULL;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\') { p++; if (!*p) return NULL; }
        p++;
    }
    return (*p == '"') ? p + 1 : NULL;
}

static const char *skip_value(const char *p);

static const char *skip_object(const char *p)
{
    if (!p || *p != '{') return NULL;
    p = skip_ws(p + 1);
    if (*p == '}') return p + 1;
    while (*p) {
        p = skip_str(p);
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p != ':') return NULL;
        p = skip_value(p + 1);
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p == '}') return p + 1;
        if (*p != ',') return NULL;
        p = skip_ws(p + 1);
    }
    return NULL;
}

static const char *skip_array(const char *p)
{
    if (!p || *p != '[') return NULL;
    p = skip_ws(p + 1);
    if (*p == ']') return p + 1;
    while (*p) {
        p = skip_value(p);
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p == ']') return p + 1;
        if (*p != ',') return NULL;
        p = skip_ws(p + 1);
    }
    return NULL;
}

static const char *skip_value(const char *p)
{
    if (!p) return NULL;
    p = skip_ws(p);
    switch (*p) {
    case '{': return skip_object(p);
    case '[': return skip_array(p);
    case '"': return skip_str(p);
    case 't': return (strncmp(p, "true",  4) == 0) ? p + 4 : NULL;
    case 'f': return (strncmp(p, "false", 5) == 0) ? p + 5 : NULL;
    case 'n': return (strncmp(p, "null",  4) == 0) ? p + 4 : NULL;
    default:
        if (*p == '-' || isdigit((unsigned char)*p)) {
            while (isdigit((unsigned char)*p) || *p == '-' || *p == '+'
                   || *p == '.' || *p == 'e' || *p == 'E')
                p++;
            return p;
        }
        return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Parse one peer JSON object                                           */
/* ------------------------------------------------------------------ */

/* Parses the peer object at *p (must start with '{').
 * Returns pointer past the closing '}' (or NULL on parse error).
 * Sets *ok=1 and fills *out only when the peer is online with an IPv4. */
static const char *parse_peer_json(const char *p, struct ts_peer *out, int *ok)
{
    *ok = 0;
    if (!p || *p != '{') return skip_object(p);
    p = skip_ws(p + 1);

    char hostname[HOSTNAME_LEN] = {0};
    char ipv4[IP_LEN]           = {0};
    int  online                 = 0;

    while (*p && *p != '}') {
        if (*p != '"') return NULL;

        char key[64];
        p = parse_str(p, key, sizeof(key));
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p != ':') return NULL;
        p = skip_ws(p + 1);

        if (strcmp(key, "HostName") == 0) {
            char tmp[HOSTNAME_LEN];
            const char *nx = parse_str(p, tmp, sizeof(tmp));
            if (nx) { SCOPY(hostname, tmp); p = nx; }
            else    { p = skip_value(p); }

        } else if (strcmp(key, "Online") == 0) {
            if (strncmp(p, "true", 4) == 0) { online = 1; p += 4; }
            else { p = skip_value(p); }

        } else if (strcmp(key, "TailscaleIPs") == 0) {
            if (*p == '[') {
                p = skip_ws(p + 1);
                while (*p && *p != ']') {
                    char ip[IP_LEN];
                    const char *nx = parse_str(p, ip, sizeof(ip));
                    if (!nx) break;
                    /* first IPv4 (no colon = not IPv6) */
                    if (ipv4[0] == '\0' && strchr(ip, ':') == NULL)
                        SCOPY(ipv4, ip);
                    p = skip_ws(nx);
                    if (*p == ',') p = skip_ws(p + 1);
                }
                if (*p == ']') p++;
            } else {
                p = skip_value(p);
            }
        } else {
            p = skip_value(p);
        }

        if (!p) return NULL;
        p = skip_ws(p);
        if (*p == ',') p = skip_ws(p + 1);
    }

    if (*p == '}') p++;

    if (hostname[0] && ipv4[0] && online) {
        SCOPY(out->hostname, hostname);
        SCOPY(out->ipv4,     ipv4);
        *ok = 1;
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* Parse the Self object (always considered online)                     */
/* ------------------------------------------------------------------ */

static int parse_self_json(const char *p, struct ts_peer *out)
{
    if (!p || *p != '{') return 0;
    p = skip_ws(p + 1);

    char hostname[HOSTNAME_LEN] = {0};
    char ipv4[IP_LEN]           = {0};

    while (*p && *p != '}') {
        if (*p != '"') break;
        char key[64];
        p = parse_str(p, key, sizeof(key));
        if (!p) break;
        p = skip_ws(p);
        if (*p != ':') break;
        p = skip_ws(p + 1);

        if (strcmp(key, "HostName") == 0) {
            char tmp[HOSTNAME_LEN];
            const char *nx = parse_str(p, tmp, sizeof(tmp));
            if (nx) { SCOPY(hostname, tmp); p = nx; }
            else    { p = skip_value(p); }
        } else if (strcmp(key, "TailscaleIPs") == 0) {
            if (*p == '[') {
                p = skip_ws(p + 1);
                while (*p && *p != ']') {
                    char ip[IP_LEN];
                    const char *nx = parse_str(p, ip, sizeof(ip));
                    if (!nx) break;
                    if (ipv4[0] == '\0' && strchr(ip, ':') == NULL)
                        SCOPY(ipv4, ip);
                    p = skip_ws(nx);
                    if (*p == ',') p = skip_ws(p + 1);
                }
                if (*p == ']') p++;
            } else {
                p = skip_value(p);
            }
        } else {
            p = skip_value(p);
        }

        if (!p) break;
        p = skip_ws(p);
        if (*p == ',') p = skip_ws(p + 1);
    }

    if (hostname[0] && ipv4[0]) {
        SCOPY(out->hostname, hostname);
        SCOPY(out->ipv4,     ipv4);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Fetch peers from tailscale                                           */
/* ------------------------------------------------------------------ */

static int fetch_peers(struct ts_peer *peers, int max)
{
    FILE *fp = popen("tailscale status --json 2>/dev/null", "r");
    if (!fp) return -1;

    char *buf = malloc(JSON_BUF_SIZE);
    if (!buf) { pclose(fp); return -1; }

    size_t total = 0, n;
    while ((n = fread(buf + total, 1, JSON_BUF_SIZE - total - 1, fp)) > 0) {
        total += n;
        if (total + 1 >= (size_t)JSON_BUF_SIZE) break;
    }
    pclose(fp);
    buf[total] = '\0';

    if (total == 0) { free(buf); return 0; }

    int count = 0;

    /* Self ---------------------------------------------------------- */
    const char *self_kw = strstr(buf, "\"Self\":");
    if (self_kw && count < max) {
        const char *p = skip_ws(self_kw + 7);
        if (*p == '{') {
            struct ts_peer self = {{0},{0}};
            if (parse_self_json(p, &self))
                peers[count++] = self;
        }
    }

    /* Peer map ------------------------------------------------------- */
    const char *peer_kw = strstr(buf, "\"Peer\":");
    if (!peer_kw) { free(buf); return count; }

    const char *p = skip_ws(peer_kw + 7);
    if (*p != '{') { free(buf); return count; }
    p = skip_ws(p + 1);

    while (*p && *p != '}' && count < max) {
        if (*p != '"') break;
        p = skip_str(p);              /* skip nodekey string */
        if (!p) break;
        p = skip_ws(p);
        if (*p != ':') break;
        p = skip_ws(p + 1);

        int ok = 0;
        struct ts_peer peer = {{0},{0}};
        p = parse_peer_json(p, &peer, &ok);
        if (!p) break;
        if (ok) peers[count++] = peer;

        p = skip_ws(p);
        if (*p == ',') p = skip_ws(p + 1);
    }

    free(buf);
    return count;
}

/* ------------------------------------------------------------------ */
/* Cache file I/O                                                       */
/* ------------------------------------------------------------------ */

static void ensure_cache_dir(void)
{
    struct stat st;
    if (stat(CACHE_DIR, &st) != 0)
        mkdir(CACHE_DIR, 01777);
}

static void write_cache(const struct ts_peer *peers, int count, time_t ts)
{
    ensure_cache_dir();

    char tmp[sizeof(CACHE_FILE) + 32];
    snprintf(tmp, sizeof(tmp), CACHE_FILE ".%d.tmp", (int)getpid());

    FILE *f = fopen(tmp, "w");
    if (!f) return;

    fprintf(f, "%ld\n", (long)ts);
    for (int i = 0; i < count; i++)
        fprintf(f, "%s %s\n", peers[i].ipv4, peers[i].hostname);
    fclose(f);

    rename(tmp, CACHE_FILE);
}

static int read_cache(struct ts_peer *peers, int max, time_t *ts_out)
{
    FILE *f = fopen(CACHE_FILE, "r");
    if (!f) return -1;

    long ts = 0;
    if (fscanf(f, "%ld\n", &ts) != 1) { fclose(f); return -1; }
    *ts_out = (time_t)ts;

    int count = 0;
    char ip[IP_LEN], host[HOSTNAME_LEN];
    while (count < max && fscanf(f, "%45s %255s\n", ip, host) == 2) {
        ip[IP_LEN - 1]       = '\0';
        host[HOSTNAME_LEN - 1] = '\0';
        SCOPY(peers[count].ipv4,     ip);
        SCOPY(peers[count].hostname, host);
        count++;
    }
    fclose(f);
    return count;
}

/* ------------------------------------------------------------------ */
/* Cache management (must be called with g_mutex held)                  */
/* ------------------------------------------------------------------ */

static void do_refresh(void)
{
    time_t now = time(NULL);
    g_last_refresh = now;

    struct ts_peer new_peers[MAX_PEERS];
    int n = fetch_peers(new_peers, MAX_PEERS);
    if (n < 0) return;

    memcpy(g_peers, new_peers, (size_t)n * sizeof(struct ts_peer));
    g_count  = n;
    g_loaded = now;

    write_cache(new_peers, n, now);
}

static void ensure_cache(void)
{
    time_t now = time(NULL);

    if (g_count > 0 && (now - g_loaded) < CACHE_TTL)
        return;

    struct ts_peer file_peers[MAX_PEERS];
    time_t file_ts = 0;
    int file_count = read_cache(file_peers, MAX_PEERS, &file_ts);

    if (file_count >= 0 && (now - file_ts) < CACHE_TTL) {
        memcpy(g_peers, file_peers, (size_t)file_count * sizeof(struct ts_peer));
        g_count  = file_count;
        g_loaded = file_ts;
        return;
    }

    do_refresh();
}

/* ------------------------------------------------------------------ */
/* Peer lookup helpers (call with g_mutex held)                         */
/* ------------------------------------------------------------------ */

static const struct ts_peer *find_by_name(const char *name)
{
    for (int i = 0; i < g_count; i++)
        if (strcasecmp(g_peers[i].hostname, name) == 0)
            return &g_peers[i];
    return NULL;
}

static const struct ts_peer *find_by_ip(const char *ip)
{
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_peers[i].ipv4, ip) == 0)
            return &g_peers[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* NSS helpers                                                          */
/* ------------------------------------------------------------------ */

static enum nss_status fill_hostent(
    const char *hostname, const char *ip_str, int af,
    struct hostent *result, char *buffer, size_t buflen,
    int *errnop, int *herrnop)
{
    size_t addrsize = (af == AF_INET) ? sizeof(struct in_addr)
                                      : sizeof(struct in6_addr);
    size_t align   = sizeof(char *);
    size_t namelen = strlen(hostname) + 1;
    size_t namepad = (namelen + align - 1) & ~(align - 1);
    /* aliases: 1 NULL pointer; addr_list: 1 ptr + NULL; addr bytes */
    size_t needed  = namepad + sizeof(char *) + 2 * sizeof(char *) + addrsize;

    if (buflen < needed) { *errnop = ERANGE; return NSS_STATUS_TRYAGAIN; }

    char *ptr = buffer;

    /* hostname */
    memcpy(ptr, hostname, namelen);
    result->h_name = ptr;
    ptr += namepad;

    /* aliases (empty) */
    char **aliases = (char **)ptr;
    aliases[0] = NULL;
    result->h_aliases = aliases;
    ptr += sizeof(char *);

    /* addr_list */
    char **addr_list = (char **)ptr;
    ptr += 2 * sizeof(char *);

    /* address bytes */
    if (af == AF_INET) {
        struct in_addr a;
        if (inet_pton(AF_INET, ip_str, &a) != 1) goto bad;
        memcpy(ptr, &a, addrsize);
    } else {
        struct in6_addr a6;
        if (inet_pton(AF_INET6, ip_str, &a6) != 1) goto bad;
        memcpy(ptr, &a6, addrsize);
    }

    addr_list[0] = ptr;
    addr_list[1] = NULL;
    result->h_addr_list = addr_list;
    result->h_addrtype  = af;
    result->h_length    = (int)addrsize;

    return NSS_STATUS_SUCCESS;

bad:
    *errnop  = EINVAL;
    *herrnop = NO_RECOVERY;
    return NSS_STATUS_UNAVAIL;
}

/* Strip .tail, look up peer, refresh once on miss if cache is old.
 * Returns NULL when name has no .tail suffix or peer not found. */
static const struct ts_peer *lookup_tail(const char *name)
{
    size_t len = strlen(name);
    if (len <= TAIL_SUFFIX_LEN) return NULL;
    if (strcasecmp(name + len - TAIL_SUFFIX_LEN, TAIL_SUFFIX) != 0) return NULL;

    char base[HOSTNAME_LEN];
    size_t blen = len - TAIL_SUFFIX_LEN;
    if (blen >= HOSTNAME_LEN) blen = HOSTNAME_LEN - 1;
    memcpy(base, name, blen);
    base[blen] = '\0';

    pthread_mutex_lock(&g_mutex);
    ensure_cache();
    const struct ts_peer *p = find_by_name(base);
    if (!p) {
        time_t now = time(NULL);
        if ((now - g_last_refresh) >= MIN_REFRESH_SEC) {
            do_refresh();
            p = find_by_name(base);
        }
    }
    pthread_mutex_unlock(&g_mutex);

    return p;
}

/* ------------------------------------------------------------------ */
/* Public NSS entry points                                              */
/* ------------------------------------------------------------------ */

enum nss_status _nss_tailscale_gethostbyname_r(
    const char *name,
    struct hostent *result, char *buffer, size_t buflen,
    int *errnop, int *herrnop)
{
    const struct ts_peer *p = lookup_tail(name);
    if (!p) { *herrnop = HOST_NOT_FOUND; return NSS_STATUS_NOTFOUND; }
    return fill_hostent(name, p->ipv4, AF_INET,
                        result, buffer, buflen, errnop, herrnop);
}

enum nss_status _nss_tailscale_gethostbyname2_r(
    const char *name, int af,
    struct hostent *result, char *buffer, size_t buflen,
    int *errnop, int *herrnop)
{
    if (af != AF_INET) { *herrnop = NO_ADDRESS; return NSS_STATUS_NOTFOUND; }
    return _nss_tailscale_gethostbyname_r(name, result, buffer, buflen,
                                          errnop, herrnop);
}

enum nss_status _nss_tailscale_gethostbyaddr_r(
    const void *addr, socklen_t addrlen, int af,
    struct hostent *result, char *buffer, size_t buflen,
    int *errnop, int *herrnop)
{
    if (af != AF_INET || addrlen != sizeof(struct in_addr)) {
        *herrnop = NO_ADDRESS;
        return NSS_STATUS_NOTFOUND;
    }

    char ip[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, addr, ip, sizeof(ip))) {
        *herrnop = NO_RECOVERY;
        return NSS_STATUS_UNAVAIL;
    }

    pthread_mutex_lock(&g_mutex);
    ensure_cache();
    const struct ts_peer *p = find_by_ip(ip);
    pthread_mutex_unlock(&g_mutex);

    if (!p) { *herrnop = HOST_NOT_FOUND; return NSS_STATUS_NOTFOUND; }

    char fullname[HOSTNAME_LEN + TAIL_SUFFIX_LEN + 1];
    snprintf(fullname, sizeof(fullname), "%s%s", p->hostname, TAIL_SUFFIX);

    return fill_hostent(fullname, ip, AF_INET,
                        result, buffer, buflen, errnop, herrnop);
}

/* Used by modern glibc getaddrinfo(3) */
enum nss_status _nss_tailscale_gethostbyname4_r(
    const char *name,
    struct gaih_addrtuple **pat,
    char *buffer, size_t buflen,
    int *errnop, int *herrnop,
    int32_t *ttlp)
{
    const struct ts_peer *p = lookup_tail(name);
    if (!p) { *herrnop = HOST_NOT_FOUND; return NSS_STATUS_NOTFOUND; }

    size_t namelen = strlen(name) + 1;
    size_t needed  = sizeof(struct gaih_addrtuple) + namelen;

    if (buflen < needed) { *errnop = ERANGE; return NSS_STATUS_TRYAGAIN; }

    struct gaih_addrtuple *at = (struct gaih_addrtuple *)buffer;
    char *namebuf = buffer + sizeof(struct gaih_addrtuple);

    memcpy(namebuf, name, namelen);
    at->next    = NULL;
    at->name    = namebuf;
    at->family  = AF_INET;
    at->scopeid = 0;
    memset(at->addr, 0, sizeof(at->addr));
    inet_pton(AF_INET, p->ipv4, at->addr);

    *pat = at;
    if (ttlp) *ttlp = CACHE_TTL;

    return NSS_STATUS_SUCCESS;
}
