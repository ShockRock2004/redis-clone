#include <stdlib.h>
#include <string.h>
#include "avl.h"

static int ht(AVL *n) { return n ? n->h : 0; }
static void upd(AVL *n) { int a = ht(n->l), b = ht(n->r); n->h = 1 + (a > b ? a : b); }
static int cmp(double s, const char *nm, AVL *n) {
    if (s < n->score) return -1;
    if (s > n->score) return 1;
    return strcmp(nm, n->name);
}
static AVL *rotr(AVL *n) { AVL *l = n->l; n->l = l->r; l->r = n; upd(n); upd(l); return l; }
static AVL *rotl(AVL *n) { AVL *r = n->r; n->r = r->l; r->l = n; upd(n); upd(r); return r; }

static AVL *balance(AVL *n) {
    upd(n);
    int d = ht(n->l) - ht(n->r);
    if (d > 1)  { if (ht(n->l->l) < ht(n->l->r)) n->l = rotl(n->l); return rotr(n); }
    if (d < -1) { if (ht(n->r->r) < ht(n->r->l)) n->r = rotr(n->r); return rotl(n); }
    return n;
}

AVL *avl_insert(AVL *n, double s, const char *nm) {
    if (!n) {
        n = calloc(1, sizeof *n);
        n->h = 1; n->score = s; n->name = strdup(nm);
        return n;
    }
    int c = cmp(s, nm, n);
    if (c < 0) n->l = avl_insert(n->l, s, nm);
    else if (c > 0) n->r = avl_insert(n->r, s, nm);
    else return n;
    return balance(n);
}

AVL *avl_remove(AVL *n, double s, const char *nm) {
    if (!n) return NULL;
    int c = cmp(s, nm, n);
    if (c < 0) n->l = avl_remove(n->l, s, nm);
    else if (c > 0) n->r = avl_remove(n->r, s, nm);
    else {
        if (!n->l || !n->r) {
            AVL *child = n->l ? n->l : n->r;
            free(n->name); free(n);
            return child;
        }
        AVL *m = n->r;
        while (m->l) m = m->l;
        free(n->name);
        n->score = m->score; n->name = strdup(m->name);
        n->r = avl_remove(n->r, m->score, m->name);
    }
    return balance(n);
}

void avl_range(AVL *n, double min, int *limit, void (*cb)(AVL *, void *), void *ud) {
    if (!n || *limit <= 0) return;
    if (n->score >= min) avl_range(n->l, min, limit, cb, ud);
    if (*limit > 0 && n->score >= min) { cb(n, ud); (*limit)--; }
    avl_range(n->r, min, limit, cb, ud);
}

void avl_free(AVL *n) {
    if (!n) return;
    avl_free(n->l); avl_free(n->r);
    free(n->name); free(n);
}
