#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "avl.h"
#include "hashtable.h"
#include "heap.h"
#include "threadpool.h"

enum { T_STR, T_ZSET };
typedef struct {
    char *key;
    int type;
    char *str;
    AVL *zroot;
    HMap zidx;
    size_t heap_idx;
} Entry;

typedef struct { int fd, dead; char r[65536]; size_t rn; char *w; size_t wn, wcap; } Conn;

static HMap db;
static Heap ttl;
static TPool pool;

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static void entry_free(void *p) {
    Entry *e = p;
    free(e->str);
    if (e->type == T_ZSET) { avl_free(e->zroot); hm_free(&e->zidx, free); }
    free(e->key); free(e);
}

static void entry_del(Entry *e) {
    if (e->heap_idx != SIZE_MAX) heap_remove(&ttl, e->heap_idx);
    hm_del(&db, e->key);
    if (e->type == T_ZSET && e->zidx.size > 1000) tp_submit(&pool, entry_free, e);
    else entry_free(e);
}

static Entry *lookup(const char *k) { void **p = hm_get(&db, k); return p ? *p : NULL; }

static Entry *create(const char *k, int type) {
    Entry *e = calloc(1, sizeof *e);
    e->key = strdup(k); e->type = type; e->heap_idx = SIZE_MAX;
    if (type == T_ZSET) hm_init(&e->zidx);
    hm_set(&db, e->key, e);
    return e;
}

static void out(Conn *c, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    if (c->wn + n > c->wcap) { c->wcap = (c->wn + n) * 2; c->w = realloc(c->w, c->wcap); }
    memcpy(c->w + c->wn, buf, n);
    c->wn += n;
}

static void key_cb(const char *k, void *v, void *c) { (void)v; out(c, "%s\n", k); }
static void zrange_cb(AVL *n, void *c) { out(c, "%s %g\n", n->name, n->score); }

static void do_cmd(Conn *c, char **a, int n) {
#define IS(s) (!strcasecmp(a[0], s))
    if (n == 0) return;
    Entry *e = n > 1 ? lookup(a[1]) : NULL;

    if (IS("PING")) out(c, "PONG\n");
    else if (IS("SET") && n == 3) {
        if (e && e->type != T_STR) { entry_del(e); e = NULL; }
        if (!e) e = create(a[1], T_STR);
        free(e->str); e->str = strdup(a[2]);
        out(c, "OK\n");
    }
    else if (IS("GET") && n == 2) {
        if (!e) out(c, "(nil)\n");
        else if (e->type != T_STR) out(c, "ERR wrong type\n");
        else out(c, "%s\n", e->str);
    }
    else if (IS("DEL") && n == 2) { if (e) entry_del(e); out(c, "%d\n", !!e); }
    else if (IS("KEYS") && n == 1) { hm_each(&db, key_cb, c); out(c, "END\n"); }
    else if (IS("EXPIRE") && n == 3) {
        if (e) heap_upsert(&ttl, &e->heap_idx, now_ms() + atoll(a[2]) * 1000);
        out(c, "%d\n", !!e);
    }
    else if (IS("PERSIST") && n == 2) {
        int had = e && e->heap_idx != SIZE_MAX;
        if (had) heap_remove(&ttl, e->heap_idx);
        out(c, "%d\n", had);
    }
    else if (IS("TTL") && n == 2) {
        if (!e) out(c, "-2\n");
        else if (e->heap_idx == SIZE_MAX) out(c, "-1\n");
        else out(c, "%lld\n", (ttl.a[e->heap_idx].at - now_ms() + 999) / 1000);
    }
    else if (IS("ZADD") && n == 4) {
        if (e && e->type != T_ZSET) { out(c, "ERR wrong type\n"); return; }
        if (!e) e = create(a[1], T_ZSET);
        double s = strtod(a[2], NULL);
        void **p = hm_get(&e->zidx, a[3]);
        if (p) {
            double *old = *p;
            e->zroot = avl_remove(e->zroot, *old, a[3]);
            *old = s;
        } else {
            double *d = malloc(sizeof *d); *d = s;
            hm_set(&e->zidx, a[3], d);
        }
        e->zroot = avl_insert(e->zroot, s, a[3]);
        out(c, "%d\n", !p);
    }
    else if (IS("ZREM") && n == 3) {
        double *d = (e && e->type == T_ZSET) ? hm_del(&e->zidx, a[2]) : NULL;
        if (d) { e->zroot = avl_remove(e->zroot, *d, a[2]); free(d); }
        out(c, "%d\n", !!d);
    }
    else if (IS("ZSCORE") && n == 3) {
        void **p = (e && e->type == T_ZSET) ? hm_get(&e->zidx, a[2]) : NULL;
        if (p) out(c, "%g\n", *(double *)*p); else out(c, "(nil)\n");
    }
    else if (IS("ZRANGEBYSCORE") && n == 4) {
        int limit = atoi(a[3]);
        if (e && e->type == T_ZSET) avl_range(e->zroot, strtod(a[2], NULL), &limit, zrange_cb, c);
        out(c, "END\n");
    }
    else out(c, "ERR unknown command or wrong number of args\n");
#undef IS
}

static void on_write(Conn *c) {
    while (c->wn) {
        ssize_t k = write(c->fd, c->w, c->wn);
        if (k < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK) c->dead = 1; return; }
        memmove(c->w, c->w + k, c->wn - k);
        c->wn -= k;
    }
}

static void on_read(Conn *c) {
    for (;;) {
        ssize_t k = read(c->fd, c->r + c->rn, sizeof c->r - c->rn);
        if (k == 0) { c->dead = 1; break; }
        if (k < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK) c->dead = 1; break; }
        c->rn += k;
        char *s = c->r, *nl;
        while ((nl = memchr(s, '\n', c->r + c->rn - s))) {
            *nl = 0;
            if (nl > s && nl[-1] == '\r') nl[-1] = 0;
            char *a[8]; int n = 0;
            for (char *t = strtok(s, " \t"); t && n < 8; t = strtok(NULL, " \t")) a[n++] = t;
            do_cmd(c, a, n);
            s = nl + 1;
        }
        c->rn -= s - c->r;
        memmove(c->r, s, c->rn);
        if (c->rn == sizeof c->r) { c->dead = 1; break; }
    }
    on_write(c);
}

static void nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 6380;
    signal(SIGPIPE, SIG_IGN);
    hm_init(&db);
    tp_init(&pool, 4);

    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) || listen(ls, SOMAXCONN)) { perror("bind/listen"); return 1; }
    nonblock(ls);
    printf("listening on :%d\n", port);

    Conn **conns = NULL;
    size_t nconns = 0;
    struct pollfd *pfd = NULL;

    for (;;) {
        pfd = realloc(pfd, (nconns + 1) * sizeof *pfd);
        size_t np = 0;
        pfd[np++] = (struct pollfd){ ls, POLLIN, 0 };
        for (size_t i = 0; i < nconns; i++)
            if (conns[i]) pfd[np++] = (struct pollfd){ (int)i, POLLIN | (conns[i]->wn ? POLLOUT : 0), 0 };

        int timeout = -1;
        if (ttl.n) { long long d = ttl.a[0].at - now_ms(); timeout = d < 0 ? 0 : (int)d; }
        if (poll(pfd, np, timeout) < 0 && errno != EINTR) { perror("poll"); return 1; }

        if (pfd[0].revents & POLLIN) {
            int fd;
            while ((fd = accept(ls, NULL, NULL)) >= 0) {
                nonblock(fd);
                if ((size_t)fd >= nconns) {
                    conns = realloc(conns, (fd + 1) * sizeof *conns);
                    memset(conns + nconns, 0, (fd + 1 - nconns) * sizeof *conns);
                    nconns = fd + 1;
                }
                conns[fd] = calloc(1, sizeof(Conn));
                conns[fd]->fd = fd;
            }
        }

        for (size_t i = 1; i < np; i++) {
            short ev = pfd[i].revents;
            if (!ev) continue;
            Conn *c = conns[pfd[i].fd];
            if (ev & POLLIN) on_read(c);
            if (!c->dead && (ev & POLLOUT)) on_write(c);
            if (c->dead || (ev & (POLLERR | POLLHUP | POLLNVAL))) {
                close(c->fd); conns[c->fd] = NULL; free(c->w); free(c);
            }
        }

        long long t = now_ms();
        while (ttl.n && ttl.a[0].at <= t)
            entry_del((Entry *)((char *)ttl.a[0].ref - offsetof(Entry, heap_idx)));
    }
}
