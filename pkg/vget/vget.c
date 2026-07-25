/* vget — VerkOS' own HTTP(S) fetcher (a curl/wget in one binary).
 *
 * Speaks HTTP/1.1 over a plain socket, and HTTPS via OpenSSL (which VerkOS
 * already ships). Handles Content-Length and chunked bodies and follows
 * redirects. Installed as the familiar `curl` (body to stdout) and `wget`
 * (body to a file).
 *
 *   curl [-s] [-L] [-I] [-k] [-o FILE] URL
 *   wget [-q] [-O FILE] URL
 *     -L follow redirects   -I headers only   -k don't verify TLS cert
 *     -o/-O FILE            -s/-q quiet
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>

static int quiet, follow, head_only, noverify;

/* a connection: plain fd, or fd wrapped in SSL */
struct conn { int fd; SSL *ssl; };
static int cread(struct conn *c, void *b, int n)  { return c->ssl ? SSL_read(c->ssl, b, n)  : (int)read(c->fd, b, n); }
static int cwrite(struct conn *c, const void *b, int n) { return c->ssl ? SSL_write(c->ssl, b, n) : (int)write(c->fd, b, n); }
static void cclose(struct conn *c) { if (c->ssl) { SSL_shutdown(c->ssl); SSL_free(c->ssl); } if (c->fd >= 0) close(c->fd); }

/* parse URL into scheme/host/port/path; returns 0 on success */
static int parse_url(const char *url, char *scheme, char *host, int *port, char *path) {
    const char *p = url;
    const char *s = strstr(url, "://");
    if (s) { size_t l = s - url; if (l > 7) l = 7; memcpy(scheme, url, l); scheme[l] = 0; p = s + 3; }
    else strcpy(scheme, "http");
    int https = !strcmp(scheme, "https");
    *port = https ? 443 : 80;
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', hostend - p);
    size_t hl = (colon ? colon : hostend) - p; if (hl > 255) hl = 255;
    memcpy(host, p, hl); host[hl] = 0;
    if (colon) *port = atoi(colon + 1);
    if (slash) snprintf(path, 1024, "%s", slash); else strcpy(path, "/");
    return 0;
}

static int tcp_open(const char *host, int port) {
    char portstr[8]; snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints, *res, *rp; memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, 0);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* read a single \n-terminated line from the connection into buf */
static int cread_line(struct conn *c, char *buf, int max) {
    int i = 0; char ch;
    while (i < max - 1) {
        int n = cread(c, &ch, 1);
        if (n <= 0) break;
        if (ch == '\r') continue;
        if (ch == '\n') break;
        buf[i++] = ch;
    }
    buf[i] = 0;
    return i;
}

/* perform one request; write body to `out`. On success returns HTTP status and,
 * for a redirect, copies the Location into `location`. */
static int request(const char *scheme, const char *host, int port, const char *path,
                   FILE *out, char *location, SSL_CTX *ctx) {
    struct conn c = { -1, NULL };
    c.fd = tcp_open(host, port);
    if (c.fd < 0) { fprintf(stderr, "vget: cannot connect to %s:%d\n", host, port); return -1; }
    if (!strcmp(scheme, "https")) {
        c.ssl = SSL_new(ctx);
        SSL_set_fd(c.ssl, c.fd);
        SSL_set_tlsext_host_name(c.ssl, host);      /* SNI */
        /* Check the name on the certificate, not just its signature chain: a
         * valid certificate for some other host is not good enough. */
        if (!noverify) {
            X509_VERIFY_PARAM *vp = SSL_get0_param(c.ssl);
            X509_VERIFY_PARAM_set_hostflags(vp, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            if (!X509_VERIFY_PARAM_set1_host(vp, host, 0)) {
                fprintf(stderr, "vget: cannot set the expected hostname\n"); cclose(&c); return -1;
            }
        }
        if (SSL_connect(c.ssl) != 1) { fprintf(stderr, "vget: TLS handshake failed\n"); cclose(&c); return -1; }
        /* A failed check must end the transfer. This used to print a warning and
         * carry on, which meant any certificate at all was accepted — and vpk
         * downloads recipes over this and runs their build() as root. Use -k to
         * opt out deliberately, the way curl and wget do. */
        long vr = SSL_get_verify_result(c.ssl);
        if (!noverify && vr != X509_V_OK) {
            fprintf(stderr, "vget: TLS certificate for %s is not trusted: %s\n",
                    host, X509_verify_cert_error_string(vr));
            fprintf(stderr, "vget: refusing to continue (use -k to override)\n");
            cclose(&c); return -1;
        }
    }

    char req[2048];
    int rl = snprintf(req, sizeof req,
        "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: vget/1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n",
        head_only ? "HEAD" : "GET", path, host);
    if (cwrite(&c, req, rl) != rl) { cclose(&c); return -1; }

    char line[4096];
    cread_line(&c, line, sizeof line);              /* status line */
    int status = 0; sscanf(line, "HTTP/%*s %d", &status);
    if (head_only && !quiet) { printf("%s\n", line); }

    int chunked = 0; long clen = -1;
    location[0] = 0;
    while (cread_line(&c, line, sizeof line) > 0) {  /* headers */
        if (head_only && !quiet) printf("%s\n", line);
        if (!strncasecmp(line, "Transfer-Encoding:", 18) && strstr(line, "chunked")) chunked = 1;
        else if (!strncasecmp(line, "Content-Length:", 15)) clen = atol(line + 15);
        else if (!strncasecmp(line, "Location:", 9)) { char *v = line + 9; while (*v == ' ') v++; snprintf(location, 1024, "%.1023s", v); }
    }
    if (head_only) { cclose(&c); return status; }

    /* body */
    if (chunked) {
        for (;;) {
            if (cread_line(&c, line, sizeof line) <= 0) break;
            long sz = strtol(line, NULL, 16);
            if (sz <= 0) break;
            while (sz > 0) {
                char b[8192]; int want = sz > (long)sizeof b ? (int)sizeof b : (int)sz;
                int n = cread(&c, b, want);
                if (n <= 0) { sz = 0; break; }
                fwrite(b, 1, n, out); sz -= n;
            }
            cread_line(&c, line, sizeof line);       /* trailing CRLF */
        }
    } else {
        long got = 0; char b[8192]; int n;
        while ((n = cread(&c, b, sizeof b)) > 0) {
            fwrite(b, 1, n, out); got += n;
            if (clen >= 0 && got >= clen) break;
        }
    }
    cclose(&c);
    return status;
}

int main(int argc, char **argv) {
    const char *me = strrchr(argv[0], '/'); me = me ? me + 1 : argv[0];
    int wget_mode = !strcmp(me, "wget");
    const char *url = NULL, *outfile = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "-q")) quiet = 1;
        else if (!strcmp(argv[i], "-L")) follow = 1;
        else if (!strcmp(argv[i], "-I")) head_only = 1;
        else if (!strcmp(argv[i], "-k")) noverify = 1;
        else if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "-O")) && i + 1 < argc) outfile = argv[++i];
        else if (argv[i][0] != '-') url = argv[i];
    }
    if (!url) { fprintf(stderr, "usage: %s [-s] [-L] [-I] [-k] [-o FILE] URL\n", me); return 1; }

    SSL_library_init(); SSL_load_error_strings();
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_default_verify_paths(ctx);
    /* Without this the handshake succeeds no matter what the peer presents;
     * loading the CA store alone verifies nothing. */
    SSL_CTX_set_verify(ctx, noverify ? SSL_VERIFY_NONE : SSL_VERIFY_PEER, NULL);
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    char scheme[8], host[256], path[1024], location[1024];
    int port;
    char cur[2048]; snprintf(cur, sizeof cur, "%s", url);

    FILE *out = stdout;
    if (wget_mode && !head_only) {
        const char *fn = outfile;
        if (!fn) { const char *b = strrchr(url, '/'); fn = (b && b[1]) ? b + 1 : "index.html"; }
        out = fopen(fn, "wb");
        if (!out) { fprintf(stderr, "vget: cannot open %s\n", fn); return 1; }
        if (!quiet) fprintf(stderr, "vget: saving to %s\n", fn);
    } else if (outfile) {
        out = fopen(outfile, "wb");
        if (!out) { fprintf(stderr, "vget: cannot open %s\n", outfile); return 1; }
    }

    int status = 0;
    for (int redir = 0; redir < 10; redir++) {
        parse_url(cur, scheme, host, &port, path);
        status = request(scheme, host, port, path, out, location, ctx);
        if (status < 0) { SSL_CTX_free(ctx); return 1; }
        if (follow && (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) && location[0]) {
            if (!quiet) fprintf(stderr, "vget: %d -> %s\n", status, location);
            if (strstr(location, "://")) snprintf(cur, sizeof cur, "%s", location);
            else snprintf(cur, sizeof cur, "%s://%s:%d%s", scheme, host, port, location);
            continue;
        }
        break;
    }
    if (out != stdout) fclose(out);
    SSL_CTX_free(ctx);
    if (!quiet && status >= 400) fprintf(stderr, "vget: HTTP %d\n", status);
    return status >= 400 ? 1 : 0;
}
