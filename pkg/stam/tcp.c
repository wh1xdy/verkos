/* stam — TCP, the client half.
 *
 * UDP proved the card and the socket layer: a datagram out, a datagram back,
 * and no state between them. TCP is the first thing in this kernel that has to
 * remember a conversation — a sequence number space, what the other end has
 * acknowledged, what we owe it, and what to do when a segment does not arrive.
 *
 * ---- what is implemented ----
 *
 * Connect, send, receive, close. Three-way handshake, cumulative
 * acknowledgement, retransmission on a timeout, and a FIN in each direction.
 * That is enough to speak to an ordinary server and be understood by it, which
 * is the only claim being made.
 *
 * ---- what is not, and why each one is a real absence ----
 *
 *   - **One segment in flight.** Send, wait for the acknowledgement, send the
 *     next. No window beyond that, no congestion control, no fast retransmit.
 *     Correct and slow: a round trip per segment.
 *   - **No reassembly.** A segment arriving out of order is dropped and the
 *     acknowledgement repeats what we still want. The sender retransmits, and
 *     over a link that reorders, throughput collapses. It does not corrupt.
 *   - **One connection per listener at a time in the backlog.** A second SYN
 *     arriving before the first is accepted is answered and queued; a fifth is
 *     dropped, and the client retransmits.
 *   - **No TIME_WAIT.** After the last acknowledgement the socket is free
 *     immediately, so a port reused within the network's memory could confuse
 *     a stale segment for a new one. The ports are ephemeral and the machine
 *     is a hypervisor guest, which makes it unlikely and not impossible.
 *   - **No options** except the maximum segment size on the SYN. No window
 *     scaling, no selective acknowledgement, no timestamps.
 *
 * Every one of those is a thing a real stack does. They are listed here rather
 * than discovered later.
 *
 * ---- sequence arithmetic ----
 *
 * Sequence numbers wrap at 2^32, so `a > b` is wrong and `(int32_t)(a - b) > 0`
 * is right: it asks which is ahead *modulo the space*, which is the only
 * question that has an answer. Every comparison here goes through seq_ge for
 * that reason.
 */
#include "net.h"
#include "tcp.h"
#include "console.h"
#include "timer.h"
#include "sched.h"
#include <stdint.h>

#define F_FIN  0x01
#define F_SYN  0x02
#define F_RST  0x04
#define F_PSH  0x08
#define F_ACK  0x10

#define NTCP       6
#define RXBUF      4096
#define MSS        1400          /* 1500 - 20 - 20, rounded down */
#define RETRIES    6
#define RTO_TICKS  1             /* one timer tick; the timer runs at 100 Hz */

enum { T_FREE = 0, T_LISTEN, T_SYN_SENT, T_SYN_RCVD,
       T_ESTABLISHED, T_CLOSE_WAIT, T_FIN_WAIT, T_CLOSED };

struct tcp_sock {
    int      state;
    uint16_t lport, rport;
    uint32_t rip;
    uint32_t snd_nxt;            /* the next sequence number we will send   */
    uint32_t snd_una;            /* the oldest we have not seen acked       */
    uint32_t rcv_nxt;            /* what we expect from them next           */
    uint8_t  rx[RXBUF];
    unsigned rxlen;
    int      fin_seen;           /* they have closed their half             */
    int      reset;              /* they sent RST; every call fails now     */
    /* For a connection that arrived rather than one we opened: which
     * listening socket it belongs to until somebody accepts it. Cleared on
     * accept, so an accepted connection is nobody's backlog. */
    struct tcp_sock *parent;
};
static struct tcp_sock socks[NTCP];
static uint16_t next_port = 40000;

static int seq_ge(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

/* ---- writing a segment --------------------------------------------------
 *
 * The header is twenty bytes and the checksum covers a pseudo-header that is
 * not in the packet: the two addresses, the protocol and the TCP length. A
 * stack that leaves it zero is not conforming — unlike UDP, where zero means
 * "not computed", TCP has no such value, and a zero checksum is simply wrong.
 */
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static int send_seg(struct tcp_sock *s, uint8_t flags, uint32_t seq,
                    const uint8_t *data, unsigned len, int with_mss) {
    uint8_t seg[20 + 4 + MSS];
    unsigned hlen = with_mss ? 24 : 20;
    for (unsigned i = 0; i < hlen; i++) seg[i] = 0;

    put16(seg + 0, s->lport);
    put16(seg + 2, s->rport);
    put32(seg + 4, seq);
    put32(seg + 8, s->rcv_nxt);
    seg[12] = (uint8_t)((hlen / 4) << 4);
    seg[13] = flags;
    /* The window is what is free in the receive buffer, which is the only
     * honest number: advertising more than we can hold is how a stack loses
     * data it told the other end to send. */
    put16(seg + 14, (uint16_t)(RXBUF - s->rxlen));
    if (with_mss) {
        seg[20] = 2; seg[21] = 4;            /* maximum segment size */
        put16(seg + 22, MSS);
    }
    for (unsigned i = 0; i < len; i++) seg[hlen + i] = data[i];

    uint16_t c = net_ip_checksum_with_pseudo(net_my_ip(), s->rip, 6,
                                             seg, hlen + len);
    put16(seg + 16, c);
    return net_ip_send(s->rip, 6, seg, hlen + len);
}

/* ---- reading one ---------------------------------------------------------
 *
 * Called from the dispatcher in net.c with the TCP header and whatever follows
 * it. Everything this function decides is decided from the segment; nothing is
 * assumed about what was sent.
 */
void tcp_input(const uint8_t *seg, unsigned n, uint32_t src) {
    if (n < 20) return;
    unsigned hlen = (unsigned)(seg[12] >> 4) * 4;
    if (hlen < 20 || hlen > n) return;
    if (net_ip_checksum_with_pseudo(src, net_my_ip(), 6, seg, n) != 0) return;

    uint16_t sport = get16(seg + 0), dport = get16(seg + 2);
    uint32_t seq = get32(seg + 4), ack = get32(seg + 8);
    uint8_t  flags = seg[13];
    const uint8_t *data = seg + hlen;
    unsigned dlen = n - hlen;

    struct tcp_sock *s = 0;
    for (int i = 0; i < NTCP; i++)
        if (socks[i].state != T_FREE && socks[i].state != T_LISTEN &&
            socks[i].lport == dport &&
            socks[i].rport == sport && socks[i].rip == src) { s = &socks[i]; break; }

    /* No connection, but perhaps somebody is listening. The four-tuple is what
     * identifies a connection, and a listener has only half of one — which is
     * why it is matched second and on the port alone. */
    if (!s && (flags & F_SYN) && !(flags & F_ACK)) {
        struct tcp_sock *l = 0;
        for (int i = 0; i < NTCP; i++)
            if (socks[i].state == T_LISTEN && socks[i].lport == dport) { l = &socks[i]; break; }
        if (!l) return;
        struct tcp_sock *c = 0;
        for (int i = 0; i < NTCP; i++)
            if (socks[i].state == T_FREE) { c = &socks[i]; break; }
        /* No room. Dropped in silence rather than refused, because a RST tells
         * the client the port is closed when it is merely busy — and a client
         * that retransmits its SYN gets in as soon as a slot frees. */
        if (!c) return;

        uint64_t t; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
        c->state    = T_SYN_RCVD;
        c->lport    = dport;
        c->rport    = sport;
        c->rip      = src;
        c->rxlen    = 0;
        c->fin_seen = 0;
        c->reset    = 0;
        c->parent   = l;
        c->rcv_nxt  = seq + 1;               /* their SYN occupies one */
        uint32_t iss = (uint32_t)t;
        c->snd_una  = iss;
        c->snd_nxt  = iss + 1;               /* and so does ours */
        send_seg(c, F_SYN | F_ACK, iss, 0, 0, 1);
        return;
    }
    if (!s) return;

    if (flags & F_RST) { s->reset = 1; s->state = T_CLOSED; return; }

    if (s->state == T_SYN_SENT) {
        if (!(flags & F_SYN) || !(flags & F_ACK)) return;
        /* A SYN occupies one sequence number even though it carries no data,
         * so the acknowledgement to ours is ISN + 1 and not ISN. Comparing
         * against ISN was the whole of the first connect failure: the segment
         * came back correct and was thrown away for not matching, and the
         * connection timed out with nothing wrong at either end. The same is
         * true of FIN, which is why close increments too. */
        if (ack != s->snd_nxt) return;
        s->snd_una = ack;
        s->rcv_nxt = seq + 1;
        s->state = T_ESTABLISHED;
        send_seg(s, F_ACK, s->snd_nxt, 0, 0, 0);
        return;
    }

    if (s->state == T_SYN_RCVD) {
        if (!(flags & F_ACK) || ack != s->snd_nxt) return;
        s->snd_una = ack;
        s->state = T_ESTABLISHED;
        /* Falls through: the same segment may carry data, and a client that
         * sends its request in the ACK — which is ordinary — would otherwise
         * have it dropped and have to retransmit. */
    }

    if (flags & F_ACK) {
        /* Cumulative: an acknowledgement covers everything before it. Only
         * moves forward, because a duplicate carries an older number and
         * accepting it would un-acknowledge data. */
        if (seq_ge(ack, s->snd_una) && seq_ge(s->snd_nxt, ack)) s->snd_una = ack;
    }

    if (dlen) {
        if (seq == s->rcv_nxt) {
            unsigned room = RXBUF - s->rxlen;
            unsigned take = dlen < room ? dlen : room;
            for (unsigned i = 0; i < take; i++) s->rx[s->rxlen + i] = data[i];
            s->rxlen += take;
            s->rcv_nxt += take;
        }
        /* Acknowledged either way, and with what we actually want next: in
         * order it moves forward, out of order it repeats, which is what tells
         * the other end to send that segment again. */
        send_seg(s, F_ACK, s->snd_nxt, 0, 0, 0);
    }

    if ((flags & F_FIN) && seq_ge(s->rcv_nxt, seq)) {
        s->rcv_nxt = seq + dlen + 1;
        s->fin_seen = 1;
        send_seg(s, F_ACK, s->snd_nxt, 0, 0, 0);
        if (s->state == T_ESTABLISHED) s->state = T_CLOSE_WAIT;
        else if (s->state == T_FIN_WAIT) s->state = T_CLOSED;
    }
}

/* ---- the calls above it -------------------------------------------------- */

tcp_sock_t *tcp_open(void) {
    for (int i = 0; i < NTCP; i++)
        if (socks[i].state == T_FREE) {
            struct tcp_sock *s = &socks[i];
            s->state = T_CLOSED;
            s->rxlen = 0; s->fin_seen = 0; s->reset = 0;
            s->lport = 0; s->rport = 0; s->rip = 0; s->parent = 0;
            return s;
        }
    return 0;
}

/* The port comes from bind, as it does on any system: listen carries a backlog
 * and not an address. Writing it the other way round would have been a socket
 * interface that is nearly the familiar one, which is worse than an unfamiliar
 * one — a program written against it would compile everywhere and work here. */
int tcp_bind(tcp_sock_t *s, uint16_t port) {
    if (!s || s->state != T_CLOSED) return -22;
    for (int i = 0; i < NTCP; i++)
        if (&socks[i] != s && socks[i].state == T_LISTEN && socks[i].lport == port)
            return -98;                                    /* -EADDRINUSE */
    s->lport = port;
    return 0;
}

int tcp_listen(tcp_sock_t *s) {
    if (!s || s->state != T_CLOSED || !net_present()) return -22;
    if (s->lport == 0) return -22;                         /* bind first */
    for (int i = 0; i < NTCP; i++)
        if (&socks[i] != s && socks[i].state == T_LISTEN && socks[i].lport == s->lport)
            return -98;
    s->rport  = 0;
    s->rip    = 0;
    s->parent = 0;
    s->state  = T_LISTEN;
    return 0;
}

uint32_t tcp_peer_ip(const tcp_sock_t *s)   { return s ? s->rip : 0; }
uint16_t tcp_peer_port(const tcp_sock_t *s) { return s ? s->rport : 0; }

/* The next connection that finished its handshake, or null at the deadline.
 *
 * Polls, like everything else that waits here: the card's frames are dispatched
 * by whoever is waiting for them, so a listener that slept without polling
 * would never see the SYN that is meant to wake it. */
tcp_sock_t *tcp_accept(tcp_sock_t *s, uint64_t ticks) {
    if (!s || s->state != T_LISTEN) return 0;
    uint64_t deadline = timer_ticks() + ticks;
    for (;;) {
        net_poll();
        for (int i = 0; i < NTCP; i++)
            if (socks[i].parent == s &&
                (socks[i].state == T_ESTABLISHED || socks[i].state == T_CLOSE_WAIT)) {
                socks[i].parent = 0;          /* accepted; nobody's backlog now */
                return &socks[i];
            }
        if (timer_ticks() >= deadline) return 0;
        net_wait(1);
    }
}

int tcp_connect(tcp_sock_t *s, uint32_t ip, uint16_t port) {
    if (!s || s->state != T_CLOSED || !net_present()) return -1;

    /* The initial sequence number has to differ between connections, or a
     * segment left over from the last one is indistinguishable from a new one.
     * The counter is the only thing here that is not the same on every boot. */
    uint64_t c; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(c));
    uint32_t iss = (uint32_t)c;
    /* A SYN occupies one sequence number even though it carries no data, so
     * the next number this end will use is one past it and the acknowledgement
     * to expect is that same number. Both of those are the same fact, and
     * writing it once here is why the handler compares against snd_nxt with no
     * arithmetic of its own. */
    s->snd_una = iss;
    s->snd_nxt = iss + 1;
    s->rcv_nxt = 0;
    s->rip   = ip;
    s->rport = port;
    s->lport = next_port++;
    if (next_port < 40000) next_port = 40000;
    s->state = T_SYN_SENT;

    for (int try = 0; try < RETRIES; try++) {
        if (send_seg(s, F_SYN, iss, 0, 0, 1) != 0) break;
        uint64_t deadline = timer_ticks() + (uint64_t)(try + 1) * 20;
        while (timer_ticks() < deadline) {
            net_poll();
            if (s->reset) { s->state = T_CLOSED; return -111; }  /* -ECONNREFUSED */
            if (s->state == T_ESTABLISHED) return 0;
            net_wait(1);
        }
        /* Retransmitted with the same sequence number, which is what makes it
         * a retransmission rather than a second connection attempt. */
    }
    s->state = T_CLOSED;
    return -110;                                                 /* -ETIMEDOUT */
}

/* Stop and wait: one segment out, then block until it is acknowledged.
 *
 * Slow by a round trip per segment and correct without a congestion window,
 * a retransmission queue or a timer per segment — none of which exist here.
 * The alternative is not a faster version of this; it is a different program.
 */
long tcp_send(tcp_sock_t *s, const uint8_t *buf, unsigned len) {
    if (!s || s->reset) return -104;                             /* -ECONNRESET */
    if (s->state != T_ESTABLISHED && s->state != T_CLOSE_WAIT) return -107;
    unsigned sent = 0;
    while (sent < len) {
        unsigned chunk = len - sent;
        if (chunk > MSS) chunk = MSS;
        uint32_t seq = s->snd_nxt;
        /* Consumed when it goes out, not when it comes back. Advancing on the
         * acknowledgement instead makes every acknowledgement look like one for
         * data this end never sent — which is exactly what the guard below
         * rejects, so the segment arrived, the server answered, and the sender
         * retransmitted until it gave up with -110. The server's log showed the
         * request three times. */
        s->snd_nxt = seq + chunk;

        int acked = 0;
        for (int try = 0; try < RETRIES && !acked; try++) {
            if (send_seg(s, F_ACK | F_PSH, seq, buf + sent, chunk, 0) != 0)
                return sent ? (long)sent : -101;
            uint64_t deadline = timer_ticks() + (uint64_t)(try + 1) * 20;
            while (timer_ticks() < deadline) {
                net_poll();
                if (s->reset) return sent ? (long)sent : -104;
                if (seq_ge(s->snd_una, seq + chunk)) { acked = 1; break; }
                net_wait(RTO_TICKS);
            }
        }
        if (!acked) { s->snd_nxt = seq; return sent ? (long)sent : -110; }
        sent += chunk;
    }
    return (long)sent;
}

/* Blocks until something arrives, the other end closes, or the deadline.
 * Returns 0 at end of stream, which is what a reader has to be able to tell
 * apart from "nothing yet" — a stack that returns 0 for both makes every
 * program that reads until end of file exit early on a slow network. */
long tcp_recv(tcp_sock_t *s, uint8_t *buf, unsigned max) {
    if (!s || s->reset) return -104;
    uint64_t deadline = timer_ticks() + timer_hz() * 5;
    for (;;) {
        net_poll();
        if (s->rxlen) {
            unsigned n = s->rxlen < max ? s->rxlen : max;
            for (unsigned i = 0; i < n; i++) buf[i] = s->rx[i];
            for (unsigned i = n; i < s->rxlen; i++) s->rx[i - n] = s->rx[i];
            s->rxlen -= n;
            return (long)n;
        }
        if (s->reset) return -104;
        if (s->fin_seen) return 0;                 /* end of stream */
        if (timer_ticks() >= deadline) return -11; /* -EAGAIN */
        net_wait(1);
    }
}

void tcp_close(tcp_sock_t *s) {
    if (!s) return;
    /* A listener has no connection to end. Freeing it also frees its port,
     * which is what a program expects when it closes the socket it listened
     * on. Connections it already handed out are unaffected: accept cleared
     * their parent. */
    if (s->state == T_LISTEN) { s->state = T_FREE; return; }
    if (s->state == T_ESTABLISHED || s->state == T_CLOSE_WAIT) {
        send_seg(s, F_ACK | F_FIN, s->snd_nxt, 0, 0, 0);
        s->snd_nxt++;
        s->state = (s->state == T_CLOSE_WAIT) ? T_CLOSED : T_FIN_WAIT;
        /* Waited for, briefly, so that the other end sees the close rather
         * than a connection that stopped answering. Not waited for long: this
         * is a close, and the caller is entitled to have it return. */
        uint64_t deadline = timer_ticks() + timer_hz() / 2;
        while (timer_ticks() < deadline && s->state != T_CLOSED) {
            net_poll();
            net_wait(1);
        }
    }
    s->state = T_FREE;
}

int tcp_state_established(const tcp_sock_t *s) {
    return s && (s->state == T_ESTABLISHED || s->state == T_CLOSE_WAIT);
}
