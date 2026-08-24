/* TCP - minimalna, ale prawdziwa implementacja
 *
 * Obsluguje obie strony: aktywne otwarcie (klient: SYN -> SYN|ACK -> ACK)
 * i pasywne (serwer: nasluch, kolejka nawiazanych polaczen), przesyl
 * danych z numerami sekwencyjnymi i potwierdzeniami, retransmisje oraz
 * zamkniecie przez FIN.
 *
 * Swiadome uproszczenia zabawkowego stosu:
 *  - okno nadawcze "wyslij i czekaj": jeden niepotwierdzony kawalek na raz,
 *    wiec nie ma kolejki retransmisji ani szacowania RTT,
 *  - segmenty spoza kolejnosci sa odrzucane (nadawca je powtorzy),
 *    dzieki czemu nie ma kolejki skladania,
 *  - brak TIME_WAIT i kontroli przeciazenia.
 * Wszystko, co widzi druga strona, jest jednak zgodne z RFC 793 -
 * po drugiej stronie stoi zwykly Linux i rozmowa dziala.
 *
 * Stan zmieniaja dwa swiaty: przerwanie karty (tcp_input) i watki
 * wolajace syscalle - kazdy dostep do tablicy polaczen idzie wiec przy
 * wylaczonych przerwaniach. Retransmisjami zajmuje sie watek 'tcpd'.
 */

#include "include/tcp.h"
#include "include/net.h"
#include "include/task.h"
#include "include/pit.h"
#include "include/printk.h"
#include "include/string.h"
#include "include/io.h"

#define TCP_CONNS     6
#define RX_BUF        4096
#define TX_BUF        1460      /* jeden segment danych na raz */
#define ACCEPT_QUEUE  4
#define MSS           1460

#define FIN 0x01
#define SYN 0x02
#define RST 0x04
#define PSH 0x08
#define ACK 0x10

#define RETRANS_MS    500       /* co ile powtarzac niepotwierdzone */
#define RETRANS_MAX   8         /* po tylu probach polaczenie pada */
#define PORT_MIN      49152

enum tcp_state {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
};

static const char *state_name[] = {
    "ZAMKNIETE", "NASLUCH", "SYN-WYSLANY", "SYN-ODEBRANY",
    "NAWIAZANE", "FIN-WAIT", "CLOSE-WAIT", "LAST-ACK",
};

struct tcp_conn {
    int used;
    int owner;                  /* pid wlasciciela, -1 = jadro */
    enum tcp_state state;
    int error;                  /* 1 = RST albo wyczerpane retransmisje */

    uint32_t remote_ip;
    uint16_t remote_port, local_port;
    uint8_t remote_mac[6];

    uint32_t snd_una;           /* najstarszy niepotwierdzony bajt */
    uint32_t snd_nxt;           /* nastepny numer do wyslania */
    uint32_t rcv_nxt;           /* czego oczekujemy od drugiej strony */
    uint16_t snd_wnd;           /* okno rozglaszane przez druga strone */

    uint8_t rx[RX_BUF];
    int rx_len;
    int fin_received;

    uint8_t tx[TX_BUF];         /* dane czekajace na potwierdzenie */
    int tx_len;
    uint8_t pending_flags;      /* SYN/FIN czekajace na potwierdzenie */
    uint32_t last_send;         /* tyknicie ostatniej wysylki */
    int retries;

    int listener;               /* dla polaczen z nasluchu: id gniazda */
    int accepted;               /* czy odebrane przez tcp_accept */
};

static struct tcp_conn conns[TCP_CONNS];
static uint16_t next_port = PORT_MIN;

/* --- pomocnicze ------------------------------------------------------- */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Porownanie numerow sekwencyjnych odporne na przewiniecie licznika */
static int seq_le(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) <= 0;
}

static uint16_t rx_window(const struct tcp_conn *c)
{
    int free = RX_BUF - c->rx_len;
    return (uint16_t)(free > 0 ? free : 0);
}

static struct tcp_conn *conn_at(int id, int owner)
{
    if (id < 0 || id >= TCP_CONNS)
        return 0;
    struct tcp_conn *c = &conns[id];
    if (!c->used || c->owner != owner)
        return 0;
    return c;
}

/* Zwalnia polaczenia po procesach, ktore juz nie zyja */
static void tcp_reap(void)
{
    for (int i = 0; i < TCP_CONNS; i++)
        if (conns[i].used && conns[i].owner >= 0 &&
            !task_alive(conns[i].owner))
            conns[i].used = 0;
}

static struct tcp_conn *conn_alloc(void)
{
    tcp_reap();
    for (int i = 0; i < TCP_CONNS; i++) {
        if (conns[i].used)
            continue;
        struct tcp_conn *c = &conns[i];
        memset(c, 0, sizeof(*c));
        c->used = 1;
        c->listener = -1;
        return c;
    }
    return 0;
}

static int conn_id(const struct tcp_conn *c)
{
    return (int)(c - conns);
}

static int port_taken(uint16_t port)
{
    for (int i = 0; i < TCP_CONNS; i++)
        if (conns[i].used && conns[i].local_port == port)
            return 1;
    return 0;
}

/* --- wysylanie segmentow ---------------------------------------------- */

static void tcp_out(struct tcp_conn *c, uint8_t flags, uint32_t seq,
                    const uint8_t *data, uint16_t len)
{
    static uint8_t seg[20 + MSS];

    if (len > MSS)
        len = MSS;

    uint32_t irq = irq_save();

    put16(seg + 0, c->local_port);
    put16(seg + 2, c->remote_port);
    put32(seg + 4, seq);
    put32(seg + 8, c->rcv_nxt);
    seg[12] = 5 << 4;                    /* naglowek 20 B, brak opcji */
    seg[13] = flags;
    put16(seg + 14, rx_window(c));
    put16(seg + 16, 0);
    put16(seg + 18, 0);
    if (len)
        memcpy(seg + 20, data, len);

    uint16_t total = (uint16_t)(20 + len);
    put16(seg + 16, net_transport_checksum(net_ip(), c->remote_ip, 6, seg,
                                           total));

    net_ip_send_mac(c->remote_ip, c->remote_mac, 6, seg, total);
    irq_restore(irq);
}

/* Powtarza to, co czeka na potwierdzenie (SYN, dane albo FIN) */
static void tcp_retransmit(struct tcp_conn *c)
{
    if (c->pending_flags & SYN) {
        tcp_out(c, c->state == TCP_SYN_RECEIVED ? (SYN | ACK) : SYN,
                c->snd_una, 0, 0);
    } else if (c->tx_len) {
        tcp_out(c, ACK | PSH, c->snd_una, c->tx, (uint16_t)c->tx_len);
    } else if (c->pending_flags & FIN) {
        tcp_out(c, ACK | FIN, c->snd_una, 0, 0);
    } else {
        return;
    }
    c->last_send = pit_ticks();
    c->retries++;
}

/* Odpowiedz RST na segment, ktory nie pasuje do zadnego polaczenia */
static void tcp_reset(uint32_t dst_ip, const uint8_t *dst_mac,
                      uint16_t local_port, uint16_t remote_port,
                      uint32_t seq, uint32_t ack, int had_ack)
{
    uint8_t seg[20];

    put16(seg + 0, local_port);
    put16(seg + 2, remote_port);
    put32(seg + 4, had_ack ? ack : 0);
    put32(seg + 8, seq);
    seg[12] = 5 << 4;
    seg[13] = had_ack ? RST : (RST | ACK);
    put16(seg + 14, 0);
    put16(seg + 16, 0);
    put16(seg + 18, 0);
    put16(seg + 16, net_transport_checksum(net_ip(), dst_ip, 6, seg, 20));

    net_ip_send_mac(dst_ip, dst_mac, 6, seg, 20);
}

/* --- odbior (kontekst przerwania karty) -------------------------------- */

static struct tcp_conn *find_conn(uint16_t local_port, uint32_t remote_ip,
                                  uint16_t remote_port)
{
    struct tcp_conn *listener = 0;

    for (int i = 0; i < TCP_CONNS; i++) {
        struct tcp_conn *c = &conns[i];
        if (!c->used || c->local_port != local_port)
            continue;
        if (c->state == TCP_LISTEN) {
            listener = c;
            continue;
        }
        if (c->remote_ip == remote_ip && c->remote_port == remote_port)
            return c;
    }
    return listener;                     /* nowe polaczenie do nasluchu */
}

/* Nowe polaczenie przychodzace: SYN trafil na gniazdo nasluchujace */
static void accept_syn(struct tcp_conn *lis, uint32_t src_ip,
                       const uint8_t *src_mac, const uint8_t *seg,
                       uint16_t src_port)
{
    int queued = 0;
    for (int i = 0; i < TCP_CONNS; i++)
        if (conns[i].used && conns[i].listener == conn_id(lis) &&
            !conns[i].accepted)
            queued++;
    if (queued >= ACCEPT_QUEUE)
        return;                          /* kolejka pelna - niech powtorzy */

    struct tcp_conn *c = conn_alloc();
    if (!c)
        return;

    c->owner = lis->owner;
    c->listener = conn_id(lis);
    c->local_port = lis->local_port;
    c->remote_ip = src_ip;
    c->remote_port = src_port;
    memcpy(c->remote_mac, src_mac, 6);   /* MAC wprost z ramki - nie ma
                                            po co pytac ARP-a o kogos,
                                            kto wlasnie sie odezwal */
    c->rcv_nxt = get32(seg + 4) + 1;
    c->snd_una = c->snd_nxt = pit_ticks() * 2654435761u;
    c->snd_wnd = get16(seg + 14);
    c->state = TCP_SYN_RECEIVED;
    c->pending_flags = SYN;

    tcp_out(c, SYN | ACK, c->snd_nxt, 0, 0);
    c->snd_nxt++;                        /* SYN zajmuje jeden numer */
    c->last_send = pit_ticks();
}

static void deliver_data(struct tcp_conn *c, const uint8_t *data, uint16_t len)
{
    if (!len)
        return;
    int space = RX_BUF - c->rx_len;
    if (len > space)
        len = (uint16_t)space;           /* reszte nadawca powtorzy */
    memcpy(c->rx + c->rx_len, data, len);
    c->rx_len += len;
    c->rcv_nxt += len;
}

void tcp_input(uint32_t src_ip, uint32_t dst_ip, const uint8_t *src_mac,
               const uint8_t *seg, uint16_t len)
{
    (void)dst_ip;
    if (len < 20)
        return;

    uint16_t src_port = get16(seg + 0);
    uint16_t dst_port = get16(seg + 2);
    uint32_t seq = get32(seg + 4);
    uint32_t ack = get32(seg + 8);
    uint16_t hlen = (uint16_t)((seg[12] >> 4) * 4);
    uint8_t flags = seg[13];
    uint16_t wnd = get16(seg + 14);

    if (hlen < 20 || hlen > len)
        return;

    const uint8_t *data = seg + hlen;
    uint16_t dlen = (uint16_t)(len - hlen);

    struct tcp_conn *c = find_conn(dst_port, src_ip, src_port);
    if (!c) {
        /* Nikt nie slucha na tym porcie - odeslij RST, tak jak wypada */
        if (!(flags & RST))              /* na RST nie odpowiada sie RST-em */
            tcp_reset(src_ip, src_mac, dst_port, src_port,
                      seq + dlen + ((flags & (SYN | FIN)) ? 1 : 0), ack,
                      flags & ACK ? 1 : 0);
        return;
    }

    if (c->state == TCP_LISTEN) {
        if (flags & SYN)
            accept_syn(c, src_ip, src_mac, seg, src_port);
        return;
    }

    if (flags & RST) {
        c->error = 1;
        c->state = TCP_CLOSED;
        return;
    }

    c->snd_wnd = wnd;

    /* Potwierdzenia: przesuwaja okno nadawcze */
    if (flags & ACK) {
        if (seq_le(c->snd_una, ack) && seq_le(ack, c->snd_nxt)) {
            if (ack != c->snd_una) {
                c->snd_una = ack;
                c->retries = 0;
                if (c->tx_len && seq_le(c->snd_una, c->snd_nxt))
                    c->tx_len = 0;       /* dane potwierdzone */
                if (c->pending_flags && seq_le(c->snd_nxt, c->snd_una))
                    c->pending_flags = 0;
            }
        }
    }

    switch (c->state) {
    case TCP_SYN_SENT:
        if ((flags & (SYN | ACK)) == (SYN | ACK) && ack == c->snd_nxt) {
            c->rcv_nxt = seq + 1;
            c->pending_flags = 0;
            c->state = TCP_ESTABLISHED;
            tcp_out(c, ACK, c->snd_nxt, 0, 0);
        }
        return;

    case TCP_SYN_RECEIVED:
        if (flags & ACK) {
            c->pending_flags = 0;
            c->state = TCP_ESTABLISHED;
        }
        break;

    default:
        break;
    }

    if (c->state != TCP_ESTABLISHED && c->state != TCP_FIN_WAIT &&
        c->state != TCP_CLOSE_WAIT && c->state != TCP_LAST_ACK)
        return;

    /* Dane przyjmujemy wylacznie w kolejnosci - segment "z przyszlosci"
       albo powtorzony odrzucamy i przypominamy, czego oczekujemy. */
    if (dlen) {
        if (seq == c->rcv_nxt) {
            deliver_data(c, data, dlen);
            tcp_out(c, ACK, c->snd_nxt, 0, 0);
        } else {
            tcp_out(c, ACK, c->snd_nxt, 0, 0);
        }
    }

    if ((flags & FIN) && seq_le(seq, c->rcv_nxt)) {
        if (!c->fin_received) {
            c->fin_received = 1;
            c->rcv_nxt++;
            tcp_out(c, ACK, c->snd_nxt, 0, 0);
            if (c->state == TCP_ESTABLISHED)
                c->state = TCP_CLOSE_WAIT;
            else if (c->state == TCP_FIN_WAIT)
                c->state = TCP_CLOSED;
        }
    }

    if (c->state == TCP_LAST_ACK && !c->pending_flags)
        c->state = TCP_CLOSED;
    if (c->state == TCP_FIN_WAIT && !c->pending_flags && c->fin_received)
        c->state = TCP_CLOSED;
}

/* --- API dla syscalli (kontekst watku) --------------------------------- */

/* Czeka, az warunek 'stan' przestanie byc przejsciowy albo minie czas */
static int wait_established(struct tcp_conn *c, uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (c->error || !c->used)
            return -1;
        if (c->state == TCP_ESTABLISHED)
            return 0;
        if (c->state == TCP_CLOSED)
            return -1;
        task_sleep(10);
        waited += 10;
        if (task_killed())
            return -1;
    }
    return -1;
}

int tcp_connect(uint32_t ip, uint16_t port, int owner, uint32_t timeout_ms)
{
    uint8_t mac[6];
    if (net_resolve_mac(ip, mac) < 0)
        return -1;

    uint32_t irq = irq_save();
    struct tcp_conn *c = conn_alloc();
    if (!c) {
        irq_restore(irq);
        return -1;
    }

    while (port_taken(next_port) || next_port < PORT_MIN)
        next_port++;

    c->owner = owner;
    c->local_port = next_port++;
    c->remote_ip = ip;
    c->remote_port = port;
    memcpy(c->remote_mac, mac, 6);
    c->snd_una = c->snd_nxt = pit_ticks() * 2654435761u;
    c->state = TCP_SYN_SENT;
    c->pending_flags = SYN;
    c->snd_wnd = MSS;

    tcp_out(c, SYN, c->snd_nxt, 0, 0);
    c->snd_nxt++;                        /* SYN zajmuje jeden numer */
    c->last_send = pit_ticks();
    int id = conn_id(c);
    irq_restore(irq);

    if (wait_established(c, timeout_ms) < 0) {
        tcp_close(id, owner);
        return -1;
    }
    return id;
}

int tcp_listen(uint16_t port, int owner)
{
    uint32_t irq = irq_save();
    if (port_taken(port)) {
        irq_restore(irq);
        return -1;
    }
    struct tcp_conn *c = conn_alloc();
    if (!c) {
        irq_restore(irq);
        return -1;
    }
    c->owner = owner;
    c->local_port = port;
    c->state = TCP_LISTEN;
    int id = conn_id(c);
    irq_restore(irq);
    return id;
}

int tcp_accept(int listener, int owner, uint32_t timeout_ms)
{
    struct tcp_conn *lis = conn_at(listener, owner);
    if (!lis || lis->state != TCP_LISTEN)
        return -1;

    uint32_t waited = 0;
    for (;;) {
        uint32_t irq = irq_save();
        for (int i = 0; i < TCP_CONNS; i++) {
            struct tcp_conn *c = &conns[i];
            if (c->used && c->listener == listener && !c->accepted &&
                c->state == TCP_ESTABLISHED) {
                c->accepted = 1;
                irq_restore(irq);
                return i;
            }
        }
        irq_restore(irq);

        if (waited >= timeout_ms)
            return -1;
        task_sleep(10);
        waited += 10;
        if (task_killed() || !lis->used)
            return -1;
    }
}

int tcp_send(int conn, int owner, const void *data, uint16_t len,
             uint32_t timeout_ms)
{
    struct tcp_conn *c = conn_at(conn, owner);
    if (!c || (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT))
        return -1;
    if (!len)
        return 0;
    if (len > TX_BUF)
        len = TX_BUF;

    uint32_t irq = irq_save();
    if (c->tx_len) {                     /* poprzedni kawalek jeszcze wisi */
        irq_restore(irq);
        return -1;
    }
    memcpy(c->tx, data, len);
    c->tx_len = len;
    c->retries = 0;
    uint32_t seq = c->snd_nxt;
    tcp_out(c, ACK | PSH, seq, c->tx, len);
    c->snd_nxt += len;
    c->last_send = pit_ticks();
    irq_restore(irq);

    /* Czekamy na potwierdzenie - retransmisjami zajmuje sie watek tcpd */
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        if (c->error || !c->used)
            return -1;
        if (!c->tx_len)
            return len;                  /* potwierdzone */
        task_sleep(10);
        waited += 10;
        if (task_killed())
            return -1;
    }
    return -1;
}

int tcp_recv(int conn, int owner, void *buf, uint16_t max, uint32_t timeout_ms)
{
    struct tcp_conn *c = conn_at(conn, owner);
    if (!c)
        return -1;

    uint32_t waited = 0;
    for (;;) {
        uint32_t irq = irq_save();
        if (c->rx_len) {
            int n = c->rx_len < max ? c->rx_len : max;
            memcpy(buf, c->rx, n);
            if (n < c->rx_len)
                memcpy(c->rx, c->rx + n, c->rx_len - n);
            c->rx_len -= n;
            irq_restore(irq);
            return n;
        }
        int done = c->fin_received || c->state == TCP_CLOSED;
        int failed = c->error;
        irq_restore(irq);

        if (failed)
            return -1;
        if (done)
            return 0;                    /* koniec strumienia */
        if (waited >= timeout_ms)
            return -1;
        task_sleep(10);
        waited += 10;
        if (task_killed() || !c->used)
            return -1;
    }
}

void tcp_close(int conn, int owner)
{
    struct tcp_conn *c = conn_at(conn, owner);
    if (!c)
        return;

    uint32_t irq = irq_save();
    if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) {
        int last = (c->state == TCP_CLOSE_WAIT);
        c->pending_flags = FIN;
        c->retries = 0;
        tcp_out(c, ACK | FIN, c->snd_nxt, 0, 0);
        c->snd_una = c->snd_nxt;
        c->snd_nxt++;
        c->last_send = pit_ticks();
        c->state = last ? TCP_LAST_ACK : TCP_FIN_WAIT;
        irq_restore(irq);

        /* Daj chwile na potwierdzenie, ale nie blokuj w nieskonczonosc */
        for (int i = 0; i < 50 && c->state != TCP_CLOSED; i++)
            task_sleep(10);
        irq = irq_save();
    }
    c->used = 0;
    irq_restore(irq);
}

/* --- watek retransmisji ------------------------------------------------ */

static void tcpd(void *arg)
{
    (void)arg;
    for (;;) {
        task_sleep(100);
        uint32_t irq = irq_save();
        for (int i = 0; i < TCP_CONNS; i++) {
            struct tcp_conn *c = &conns[i];
            if (!c->used || c->state == TCP_LISTEN || c->state == TCP_CLOSED)
                continue;
            if (!c->tx_len && !c->pending_flags)
                continue;
            if (pit_ticks() - c->last_send < RETRANS_MS / 10)
                continue;

            if (c->retries >= RETRANS_MAX) {
                c->error = 1;
                c->state = TCP_CLOSED;
                continue;
            }
            tcp_retransmit(c);
        }
        irq_restore(irq);
    }
}

void tcp_init(void)
{
    task_create("tcpd", tcpd, 0);
}

void tcp_dump(void)
{
    int shown = 0;
    for (int i = 0; i < TCP_CONNS; i++) {
        struct tcp_conn *c = &conns[i];
        if (!c->used)
            continue;
        printk("  %d  port %u  %s", i, c->local_port, state_name[c->state]);
        if (c->state != TCP_LISTEN) {
            printk("  <-> ");
            net_print_ip(c->remote_ip);
            printk(":%u  odebrane %u B", c->remote_port, (uint32_t)c->rx_len);
        }
        printk("\n");
        shown++;
    }
    if (!shown)
        printk("brak polaczen TCP\n");
}
