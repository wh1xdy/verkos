/* vwhois — VerkOS' own whois client.
 *
 * Queries a whois server (port 43) for a domain or IP. With no -h it starts at
 * whois.iana.org and follows the "refer:"/"whois:" referral to the registry's
 * own server, then prints that answer. Installed as `whois`.
 *
 *   whois [-h SERVER] QUERY
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* query `server` for `q`; append the response to `out` (size *outlen), and if a
 * referral server is found, copy it into `refer`. Returns 0 on success. */
static int whois_query(const char *server, const char *q, char **out, size_t *outlen, char *refer) {
    struct addrinfo hints, *res, *rp; memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(server, "43", &hints, &res) != 0) { fprintf(stderr, "whois: cannot resolve %s\n", server); return -1; }
    int s = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        s = socket(rp->ai_family, rp->ai_socktype, 0);
        if (s >= 0 && connect(s, rp->ai_addr, rp->ai_addrlen) == 0) break;
        if (s >= 0) { close(s); s = -1; }
    }
    freeaddrinfo(res);
    if (s < 0) { fprintf(stderr, "whois: cannot connect to %s\n", server); return -1; }

    char req[512]; int rl = snprintf(req, sizeof req, "%s\r\n", q);
    if (write(s, req, rl) != rl) { close(s); return -1; }

    if (refer) refer[0] = 0;
    char buf[4096]; ssize_t n;
    while ((n = read(s, buf, sizeof buf)) > 0) {
        *out = realloc(*out, *outlen + n + 1);
        memcpy(*out + *outlen, buf, n); *outlen += n; (*out)[*outlen] = 0;
        if (refer && !refer[0]) {
            char *r = strstr(*out, "refer:");
            if (!r) r = strstr(*out, "whois:");
            if (r) { char srv[256]; if (sscanf(r, "%*[^:]: %255s", srv) == 1) snprintf(refer, 256, "%s", srv); }
        }
    }
    close(s);
    return 0;
}

int main(int argc, char **argv) {
    const char *server = NULL, *query = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") && i + 1 < argc) server = argv[++i];
        else if (argv[i][0] != '-') query = argv[i];
    }
    if (!query) { fprintf(stderr, "usage: whois [-h SERVER] QUERY\n"); return 1; }

    char *out = NULL; size_t outlen = 0; char refer[256] = "";
    if (server) {                                    /* explicit -h: one shot */
        if (whois_query(server, query, &out, &outlen, NULL) < 0) return 1;
        if (out) fwrite(out, 1, outlen, stdout);
        free(out);
        return 0;
    }

    /* Ask IANA which registry owns this. For a domain use its TLD (the label
     * after the last dot); for an IP (all digits/dots) use the address itself. */
    int is_ip = 1;
    for (const char *c = query; *c; c++) if (*c != '.' && (*c < '0' || *c > '9')) { is_ip = 0; break; }
    const char *iana_q = query;
    if (!is_ip) { const char *dot = strrchr(query, '.'); if (dot) iana_q = dot + 1; }

    if (whois_query("whois.iana.org", iana_q, &out, &outlen, refer) < 0) return 1;
    if (refer[0]) {                                  /* follow the referral, query the FULL name */
        char *out2 = NULL; size_t l2 = 0;
        if (whois_query(refer, query, &out2, &l2, NULL) == 0 && out2) {
            fwrite(out2, 1, l2, stdout); free(out2); free(out); return 0;
        }
        free(out2);
    }
    if (out) fwrite(out, 1, outlen, stdout);
    free(out);
    return 0;
}
