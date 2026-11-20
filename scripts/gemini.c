/*
 * gemini.c - a simple Gemini protocol browser in C
 *
 * For Muse's own use: fetch a gemini:// URL and print it as readable
 * text, so a Gemini page can be summarised just like an https:// page.
 *
 * Usage:
 *   gemini <gemini://url>          single fetch, render, exit (default)
 *   gemini -r <gemini://url>       single fetch, print raw body
 *   gemini -i <gemini://url>       interactive: follow links by number
 *   gemini                         same as gemini gemini://geminiprotocol.net/
 *
 * Build:
 *   gcc -Wall -O2 -o gemini gemini.c -lssl -lcrypto
 *
 * Protocol (RFC draft, gemini://):
 *   Request:  absolute URL + CRLF, max 1024 bytes, over TLS on port 1965
 *   Response: <status:2 digits> SP <meta> CRLF, then body until close
 *   10/11 input, 20 success, 30/31 redirect, 40-49 temp fail,
 *   50-59 perm fail, 60-69 client cert required
 *
 * TLS note: Gemini capsules almost all use self-signed certificates
 * and trust-on-first-use. Verification is therefore disabled here.
 * Do not send passwords or other sensitive data over it, and do not
 * use this client for anything where a forged capsule would matter.
 *
 * Network note: direct TCP to port 1965 is blocked in the Muse VM.
 * The client therefore honours https_proxy / HTTPS_PROXY (and the
 * http_proxy / all_proxy fallbacks) and opens an HTTP CONNECT tunnel
 * to host:1965 through that proxy before starting TLS, exactly the
 * way HTTPS access works here. With no proxy variable set it connects
 * directly, as a normal Gemini client would.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_URL 2048
#define MAX_HOST 256
#define MAX_META 1025
#define MAX_LINKS 200
#define MAX_REDIRECTS 5
#define TIMEOUT_SEC 15

static char *links[MAX_LINKS];
static int nlinks = 0;
static int raw_mode = 0;

static void free_links(void) {
    for (int i = 0; i < nlinks; i++) free(links[i]);
    nlinks = 0;
}

static void add_link(const char *url) {
    if (nlinks >= MAX_LINKS) return;
    links[nlinks] = strdup(url);
    if (links[nlinks]) nlinks++;
}

/* Percent-encode a query string for an input (status 10/11) reply. */
static void url_encode(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 4 < outsz; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else if (c == ' ') {
            out[o++] = '%'; out[o++] = '2'; out[o++] = '0';
        } else {
            snprintf(out + o, outsz - o, "%%%02X", c);
            o += 3;
        }
    }
    out[o] = '\0';
}

typedef struct {
    char host[MAX_HOST];
    int port;
    char path[MAX_URL];
    char raw[MAX_URL];
} Url;

/* Parse gemini://host[:port][/path][?query] into its parts. */
static int parse_url(const char *s, Url *u) {
    memset(u, 0, sizeof(*u));
    u->port = 1965;
    const char *p = s;
    if (strncmp(p, "gemini://", 9) == 0) p += 9;
    else if (strncmp(p, "gemini:", 7) == 0) p += 7;
    const char *slash = strchr(p, '/');
    const char *q = strchr(p, '?');
    const char *host_end = slash ? slash : (q ? q : p + strlen(p));
    const char *colon = memchr(p, ':', (size_t)(host_end - p));
    if (colon) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= MAX_HOST) return -1;
        memcpy(u->host, p, hlen);
        u->host[hlen] = '\0';
        u->port = atoi(colon + 1);
        if (u->port <= 0 || u->port > 65535) return -1;
    } else {
        size_t hlen = (size_t)(host_end - p);
        if (hlen == 0 || hlen >= MAX_HOST) return -1;
        memcpy(u->host, p, hlen);
        u->host[hlen] = '\0';
    }
    if (slash) snprintf(u->path, sizeof(u->path), "%s", slash);
    else if (q) snprintf(u->path, sizeof(u->path), "/%s", q);
    else snprintf(u->path, sizeof(u->path), "/");
    /* Rebuild the canonical absolute URL used for the request. */
    if (u->port == 1965)
        snprintf(u->raw, sizeof(u->raw), "gemini://%s%s", u->host, u->path);
    else
        snprintf(u->raw, sizeof(u->raw), "gemini://%s:%d%s", u->host, u->port, u->path);
    return 0;
}

/* Resolve a link from a page against the page it came from. */
static void resolve_url(const Url *base, const char *link, char *out, size_t outsz) {
    if (strncmp(link, "gemini://", 9) == 0) {
        snprintf(out, outsz, "%s", link);
        return;
    }
    if (strncmp(link, "//", 2) == 0) {
        snprintf(out, outsz, "gemini:%s", link);
        return;
    }
    if (link[0] == '/') {
        if (base->port == 1965)
            snprintf(out, outsz, "gemini://%s%s", base->host, link);
        else
            snprintf(out, outsz, "gemini://%s:%d%s", base->host, base->port, link);
        return;
    }
    if (link[0] == '?') {
        /* Same path, new query: strip any old query from the base path. */
        char path_noq[MAX_URL];
        snprintf(path_noq, sizeof(path_noq), "%s", base->path);
        char *oldq = strchr(path_noq, '?');
        if (oldq) *oldq = '\0';
        if (base->port == 1965)
            snprintf(out, outsz, "gemini://%s%s%s", base->host, path_noq, link);
        else
            snprintf(out, outsz, "gemini://%s:%d%s%s", base->host, base->port, path_noq, link);
        return;
    }
    /* Relative link: replace the last segment of the base path. */
    char dir[MAX_URL];
    snprintf(dir, sizeof(dir), "%s", base->path);
    char *last = strrchr(dir, '/');
    if (last) last[1] = '\0';
    else snprintf(dir, sizeof(dir), "/");
    if (base->port == 1965)
        snprintf(out, outsz, "gemini://%s%s%s", base->host, dir, link);
    else
        snprintf(out, outsz, "gemini://%s:%d%s%s", base->host, base->port, dir, link);
}

/* ---- HTTP CONNECT proxy support ---------------------------------------
 * This VM blocks direct TCP to port 1965. HTTPS traffic works because
 * it goes through the egress proxy in https_proxy / HTTPS_PROXY, so
 * Gemini must do the same: CONNECT host:1965 through that proxy, then
 * TLS over the tunnel. On a machine with direct internet this code
 * is skipped entirely when no proxy variable is set. Credentials are
 * read from the environment at run time, never hard-coded or logged.
 * ---------------------------------------------------------------------- */

typedef struct {
    char host[MAX_HOST];
    int port;
    char user[128];
    char pass[256];
    int has_auth;
} Proxy;

static void base64_encode(const unsigned char *in, size_t len, char *out, size_t outsz) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len && o + 5 < outsz; i += 3) {
        int n = (int)((len - i < 3) ? len - i : 3);
        unsigned int v = in[i] << 16;
        if (n > 1) v |= in[i + 1] << 8;
        if (n > 2) v |= in[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = (n > 1) ? tbl[(v >> 6) & 63] : '=';
        out[o++] = (n > 2) ? tbl[v & 63] : '=';
    }
    out[o] = '\0';
}

/* Parse http://[user:pass@]host[:port] into Proxy. Returns 0 on success. */
static int parse_proxy_url(const char *s, Proxy *p) {
    memset(p, 0, sizeof(*p));
    p->port = 3128;
    const char *q = strstr(s, "://");
    if (q) q += 3; else q = s;
    const char *at = strchr(q, '@');
    const char *hostpart = q;
    if (at) {
        const char *colon = memchr(q, ':', (size_t)(at - q));
        if (colon) {
            size_t ulen = (size_t)(colon - q);
            size_t plen = (size_t)(at - colon - 1);
            if (ulen >= sizeof(p->user) || plen >= sizeof(p->pass)) return -1;
            memcpy(p->user, q, ulen); p->user[ulen] = '\0';
            memcpy(p->pass, colon + 1, plen); p->pass[plen] = '\0';
            p->has_auth = 1;
        }
        hostpart = at + 1;
    }
    const char *slash = strchr(hostpart, '/');
    const char *hend = slash ? slash : hostpart + strlen(hostpart);
    const char *colon = memchr(hostpart, ':', (size_t)(hend - hostpart));
    if (colon) {
        size_t hlen = (size_t)(colon - hostpart);
        if (hlen >= MAX_HOST) return -1;
        memcpy(p->host, hostpart, hlen); p->host[hlen] = '\0';
        p->port = atoi(colon + 1);
        if (p->port <= 0 || p->port > 65535) return -1;
    } else {
        size_t hlen = (size_t)(hend - hostpart);
        if (hlen == 0 || hlen >= MAX_HOST) return -1;
        memcpy(p->host, hostpart, hlen); p->host[hlen] = '\0';
    }
    return 0;
}

static int host_in_no_proxy(const char *host) {
    const char *np = getenv("no_proxy");
    if (!np) np = getenv("NO_PROXY");
    if (!np || !*np) return 0;
    if (strcmp(host, "localhost") == 0) return 1;
    char *copy = strdup(np);
    if (!copy) return 0;
    int found = 0;
    for (char *tok = strtok(copy, ","); tok; tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;
        if (*tok == '\0') continue;
        if (strcmp(tok, host) == 0) found = 1;
        else if (tok[0] == '.' && strlen(host) > strlen(tok) &&
                 strcmp(host + strlen(host) - strlen(tok), tok) == 0) found = 1;
        else if (strlen(host) > strlen(tok) + 1 &&
                 strcmp(host + strlen(host) - strlen(tok), tok) == 0 &&
                 host[strlen(host) - strlen(tok) - 1] == '.') found = 1;
        if (found) break;
    }
    free(copy);
    return found;
}

/* Return 1 and fill p when a proxy should be used for host, else 0. */
static int proxy_for_host(const char *host, Proxy *p) {
    if (host_in_no_proxy(host)) return 0;
    const char *envs[] = { "https_proxy", "HTTPS_PROXY", "http_proxy", "HTTP_PROXY",
                           "all_proxy", "ALL_PROXY", NULL };
    for (int i = 0; envs[i]; i++) {
        const char *v = getenv(envs[i]);
        if (v && *v && parse_proxy_url(v, p) == 0) return 1;
    }
    return 0;
}

static int tcp_connect(const char *host, int port) {
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = { TIMEOUT_SEC, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Connect to host:port, via an HTTP CONNECT proxy when one is configured. */
static int connect_gemini_host(const char *host, int port) {
    Proxy px;
    if (!proxy_for_host(host, &px)) {
        return tcp_connect(host, port);
    }
    int fd = tcp_connect(px.host, px.port);
    if (fd < 0) {
        fprintf(stderr, "gemini: cannot connect to proxy %s:%d\n", px.host, px.port);
        return -1;
    }
    char req[1024];
    if (px.has_auth) {
        char cred[384];
        snprintf(cred, sizeof(cred), "%s:%s", px.user, px.pass);
        char b64[512];
        base64_encode((unsigned char *)cred, strlen(cred), b64, sizeof(b64));
        snprintf(req, sizeof(req),
                 "CONNECT %s:%d HTTP/1.1\r\nHost: %s:%d\r\n"
                 "Proxy-Authorization: Basic %s\r\nProxy-Connection: Keep-Alive\r\n\r\n",
                 host, port, host, port, b64);
    } else {
        snprintf(req, sizeof(req),
                 "CONNECT %s:%d HTTP/1.1\r\nHost: %s:%d\r\nProxy-Connection: Keep-Alive\r\n\r\n",
                 host, port, host, port);
    }
    if (send(fd, req, strlen(req), 0) < 0) { close(fd); return -1; }
    /* Read the proxy response headers until the blank line. */
    char resp[4096];
    size_t n = 0;
    while (n + 1 < sizeof(resp)) {
        ssize_t r = recv(fd, resp + n, 1, 0);
        if (r <= 0) break;
        n += (size_t)r;
        resp[n] = '\0';
        if (n >= 4 && strcmp(resp + n - 4, "\r\n\r\n") == 0) break;
    }
    if (n < 12 || strncmp(resp, "HTTP/1.1 200", 12) != 0) {
        resp[n] = '\0';
        fprintf(stderr, "gemini: proxy refused CONNECT to %s:%d: %.80s\n", host, port, resp);
        close(fd);
        return -1;
    }
    return fd;
}

/* Read one CRLF-terminated line from TLS, without the CRLF. */
static int ssl_read_line(SSL *ssl, char *buf, size_t bufsz) {
    size_t n = 0;
    char c;
    while (n + 1 < bufsz) {
        int r = SSL_read(ssl, &c, 1);
        if (r <= 0) return (n == 0) ? -1 : (int)n;
        if (c == '\r') continue;
        if (c == '\n') break;
        buf[n++] = c;
    }
    buf[n] = '\0';
    return (int)n;
}

/*
 * Fetch one URL. Follows redirects, prompts for input in interactive
 * mode, and on success returns a malloc'd body in *body_out with its
 * length and MIME type in *mime_out. Returns 0 on success, else -1.
 * On a non-success status it prints the status and meta to stderr
 * (and, for input, to stdout in a form the caller can act on).
 */
static int fetch(const char *url_str, int interactive,
                 char **body_out, size_t *len_out, char *mime_out, size_t mime_sz,
                 char *final_url, size_t final_sz) {
    char current[MAX_URL];
    snprintf(current, sizeof(current), "%s", url_str);

    for (int hop = 0; hop <= MAX_REDIRECTS; hop++) {
        Url u;
        if (parse_url(current, &u) != 0) {
            fprintf(stderr, "gemini: bad URL: %s\n", current);
            return -1;
        }
        snprintf(final_url, final_sz, "%s", u.raw);

        /* Direct TCP is blocked in this VM, so this goes through the
         * egress proxy when https_proxy is set, exactly as HTTPS does. */
        int fd = connect_gemini_host(u.host, u.port);
        if (fd < 0) {
            fprintf(stderr, "gemini: cannot connect to %s:%d\n", u.host, u.port);
            return -1;
        }

        SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
        if (!ctx) { close(fd); return -1; }
        /* TOFU: most Gemini servers use self-signed certs. */
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
        SSL *ssl = SSL_new(ctx);
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, u.host);
        if (SSL_connect(ssl) != 1) {
            unsigned long e = ERR_get_error();
            fprintf(stderr, "gemini: TLS failed for %s: %s\n", u.host,
                    e ? ERR_error_string(e, NULL) : "unknown error");
            SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
            return -1;
        }

        char req[MAX_URL + 4];
        snprintf(req, sizeof(req), "%s\r\n", u.raw);
        if (SSL_write(ssl, req, (int)strlen(req)) <= 0) {
            fprintf(stderr, "gemini: failed to send request\n");
            SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
            return -1;
        }

        char header[MAX_META + 4];
        if (ssl_read_line(ssl, header, sizeof(header)) < 0) {
            fprintf(stderr, "gemini: no response header\n");
            SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
            return -1;
        }
        if (strlen(header) < 2 || !isdigit((unsigned char)header[0]) ||
            !isdigit((unsigned char)header[1])) {
            fprintf(stderr, "gemini: bad response header: %s\n", header);
            SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
            return -1;
        }
        int status = (header[0] - '0') * 10 + (header[1] - '0');
        const char *meta = header + 2;
        while (*meta == ' ') meta++;

        if (status == 30 || status == 31) {
            /* Redirect: meta is the new URL, absolute or relative. */
            char next[MAX_URL];
            resolve_url(&u, meta, next, sizeof(next));
            fprintf(stderr, "gemini: redirect %d to %s\n", status, next);
            SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
            snprintf(current, sizeof(current), "%s", next);
            continue;
        }

        if (status == 10 || status == 11) {
            if (!interactive) {
                printf("INPUT REQUIRED: %s\n", meta);
                printf("Re-run with the answer appended as ?answer, e.g. %s?answer\n", u.raw);
                SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
                return -2;
            }
            printf("Input%s: %s\n> ", status == 11 ? " (sensitive, not echoed in history)" : "", meta);
            fflush(stdout);
            char answer[1024];
            if (!fgets(answer, sizeof(answer), stdin)) {
                SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
                return -1;
            }
            answer[strcspn(answer, "\r\n")] = '\0';
            char enc[3072];
            url_encode(answer, enc, sizeof(enc));
            char next[MAX_URL];
            snprintf(next, sizeof(next), "%s?%s", u.raw, enc);
            SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
            snprintf(current, sizeof(current), "%s", next);
            continue;
        }

        if (status != 20) {
            fprintf(stderr, "gemini: %d %s\n", status, meta);
            SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
            return -1;
        }

        /* Success: read the body until the server closes the connection. */
        snprintf(mime_out, mime_sz, "%s", meta[0] ? meta : "text/gemini");
        size_t cap = 65536, len = 0;
        char *body = malloc(cap);
        if (!body) {
            SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
            return -1;
        }
        for (;;) {
            if (len + 8192 > cap) {
                cap *= 2;
                char *nb = realloc(body, cap);
                if (!nb) { free(body); SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); close(fd); return -1; }
                body = nb;
            }
            int r = SSL_read(ssl, body + len, 8192);
            if (r <= 0) break;
            len += (size_t)r;
        }
        if (len + 1 > cap) body = realloc(body, len + 1);
        body[len] = '\0';
        *body_out = body;
        *len_out = len;
        SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); close(fd);
        return 0;
    }
    fprintf(stderr, "gemini: too many redirects\n");
    return -1;
}

/* Render text/gemini, collecting => links in order for follow-up. */
static void render_gemini(const char *body) {
    free_links();
    int in_pre = 0;
    const char *p = body;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        char line[4096];
        memcpy(line, p, linelen);
        line[linelen] = '\0';
        if (linelen && line[linelen - 1] == '\r') line[--linelen] = '\0';

        if (strncmp(line, "```", 3) == 0) {
            in_pre = !in_pre;
            printf("%s\n", in_pre ? "```" : "```");
        } else if (in_pre) {
            printf("%s\n", line);
        } else if (strncmp(line, "=>", 2) == 0) {
            const char *s = line + 2;
            while (*s == ' ' || *s == '\t') s++;
            const char *end = s;
            while (*end && *end != ' ' && *end != '\t') end++;
            char url[MAX_URL];
            size_t ulen = (size_t)(end - s);
            if (ulen >= sizeof(url)) ulen = sizeof(url) - 1;
            memcpy(url, s, ulen); url[ulen] = '\0';
            while (*end == ' ' || *end == '\t') end++;
            add_link(url);
            if (*end) printf("[%d] %s (%s)\n", nlinks, end, url);
            else printf("[%d] %s\n", nlinks, url);
        } else if (strncmp(line, "### ", 4) == 0) {
            printf("\n### %s\n", line + 4);
        } else if (strncmp(line, "## ", 3) == 0) {
            printf("\n## %s\n", line + 3);
        } else if (strncmp(line, "# ", 2) == 0) {
            printf("\n# %s\n", line + 2);
        } else if (strncmp(line, "* ", 2) == 0) {
            printf("  - %s\n", line + 2);
        } else if (strncmp(line, "> ", 2) == 0) {
            printf("  > %s\n", line + 2);
        } else {
            printf("%s\n", line);
        }
        if (!eol) break;
        p = eol + 1;
    }
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [-r] [-i] [gemini://url]\n"
        "  (no flags)  fetch once, render text/gemini, exit\n"
        "  -r          fetch once, print the raw body\n"
        "  -i          interactive: type a link number, a URL, b for back, q to quit\n",
        prog);
}

int main(int argc, char **argv) {
    SSL_library_init();
    SSL_load_error_strings();

    int interactive = 0;
    const char *url = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0) interactive = 1;
        else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--raw") == 0) raw_mode = 1;
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "gemini: unknown option %s\n", argv[i]);
            print_usage(argv[0]);
            return 2;
        } else if (!url) url = argv[i];
        else { print_usage(argv[0]); return 2; }
    }
    if (!url) url = "gemini://geminiprotocol.net/";

    /* History for interactive back navigation. */
    char *history[64];
    int nhist = 0;
    char current[MAX_URL];
    snprintf(current, sizeof(current), "%s", url);

    for (;;) {
        char *body = NULL;
        size_t len = 0;
        char mime[1024] = "";
        char final_url[MAX_URL] = "";
        int rc = fetch(current, interactive, &body, &len, mime, sizeof(mime),
                       final_url, sizeof(final_url));
        if (rc != 0) {
            /* -2 is "input required" in non-interactive mode: already printed. */
            return rc == -2 ? 2 : 1;
        }
        printf("URL: %s\nMIME: %s\n\n", final_url, mime);
        if (raw_mode) {
            fwrite(body, 1, len, stdout);
            if (len && body[len - 1] != '\n') printf("\n");
        } else if (strncmp(mime, "text/gemini", 11) == 0) {
            render_gemini(body);
        } else if (strncmp(mime, "text/", 5) == 0) {
            fwrite(body, 1, len, stdout);
            if (len && body[len - 1] != '\n') printf("\n");
        } else {
            printf("[non-text content: %s, %zu bytes, not shown]\n", mime, len);
        }
        free(body);

        if (!interactive) return 0;

        /* Interactive loop. */
        printf("\n[%d link%s] number, URL, b=back, q=quit > ", nlinks, nlinks == 1 ? "" : "s");
        fflush(stdout);
        char input[2048];
        if (!fgets(input, sizeof(input), stdin)) return 0;
        input[strcspn(input, "\r\n")] = '\0';
        if (strcmp(input, "q") == 0 || strcmp(input, "quit") == 0) return 0;
        if (strcmp(input, "b") == 0 || strcmp(input, "back") == 0) {
            if (nhist > 0) {
                char *prev = history[--nhist];
                snprintf(current, sizeof(current), "%s", prev);
                free(prev);
                continue;
            }
            printf("No previous page.\n");
            continue;
        }
        if (input[0] == '\0') continue;
        char next[MAX_URL];
        char *endp = NULL;
        long num = strtol(input, &endp, 10);
        if (endp && *endp == '\0' && num >= 1 && num <= nlinks) {
            Url base;
            if (parse_url(final_url, &base) != 0) { printf("Bad current URL.\n"); continue; }
            resolve_url(&base, links[num - 1], next, sizeof(next));
        } else if (strncmp(input, "gemini://", 9) == 0 || input[0] == '/') {
            Url base;
            if (parse_url(final_url, &base) != 0) { printf("Bad current URL.\n"); continue; }
            resolve_url(&base, input, next, sizeof(next));
        } else {
            /* Bare hostname: treat it as a Gemini URL. */
            snprintf(next, sizeof(next), "gemini://%s/", input);
        }
        if (nhist < 64) history[nhist++] = strdup(current);
        snprintf(current, sizeof(current), "%s", next);
    }
}
