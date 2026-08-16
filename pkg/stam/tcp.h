#ifndef STAM_TCP_H
#define STAM_TCP_H
#include <stdint.h>

/* A connection. See tcp.c for what this implementation does and — at greater
 * length, because it matters more — what it does not. */
typedef struct tcp_sock tcp_sock_t;

tcp_sock_t *tcp_open(void);
/* Take a port. As everywhere else, the address is bind's and listen carries
 * only a backlog. 0, or -98 if something already listens there. */
int  tcp_bind(tcp_sock_t *s, uint16_t port);
int  tcp_listen(tcp_sock_t *s);
/* Who is at the other end, for accept to report. */
uint32_t tcp_peer_ip(const tcp_sock_t *s);
uint16_t tcp_peer_port(const tcp_sock_t *s);
/* The next connection that finished its handshake, or null at the deadline
 * (in timer ticks). The returned socket is a connection of its own. */
tcp_sock_t *tcp_accept(tcp_sock_t *s, uint64_t ticks);
/* Three-way handshake. 0, or a negative errno: -111 refused, -110 timed out. */
int  tcp_connect(tcp_sock_t *s, uint32_t ip, uint16_t port);
/* Stop and wait: every segment is acknowledged before the next goes out. */
long tcp_send(tcp_sock_t *s, const uint8_t *buf, unsigned len);
/* Blocks. 0 means the other end closed — which a reader must be able to tell
 * from "nothing yet", so "nothing yet" is -11 and not 0. */
long tcp_recv(tcp_sock_t *s, uint8_t *buf, unsigned max);
void tcp_close(tcp_sock_t *s);
int  tcp_state_established(const tcp_sock_t *s);

/* From the dispatcher in net.c: one segment, with the sender's address. */
void tcp_input(const uint8_t *seg, unsigned n, uint32_t src);
#endif
