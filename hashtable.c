#include <stdlib.h>
#include <string.h>
#include "hashtable.h"

static size_t hash(const char *s) {
    size_t h = 1469598103934665603ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

void hm_init(HMap *m) { m->cap = 16; m->size = 0; m->tab = calloc(m->cap, sizeof(HNode *)); }

static void grow(HMap *m) {
    size_t nc = m->cap * 2;
    HNode **nt = calloc(nc, sizeof(HNode *));
    for (size_t i = 0; i < m->cap; i++)
        for (HNode *n = m->tab[i], *nx; n; n = nx) {
            nx = n->next;
            size_t b = hash(n->key) & (nc - 1);
            n->next = nt[b]; nt[b] = n;
        }
    free(m->tab); m->tab = nt; m->cap = nc;
}

void **hm_get(HMap *m, const char *k) {
    for (HNode *n = m->tab[hash(k) & (m->cap - 1)]; n; n = n->next)
        if (!strcmp(n->key, k)) return &n->val;
    return NULL;
}

void hm_set(HMap *m, const char *k, void *v) {
    void **p = hm_get(m, k);
    if (p) { *p = v; return; }
    if (m->size >= m->cap) grow(m);
    HNode *n = malloc(sizeof *n);
    n->key = strdup(k); n->val = v;
    size_t b = hash(k) & (m->cap - 1);
    n->next = m->tab[b]; m->tab[b] = n; m->size++;
}

void *hm_del(HMap *m, const char *k) {
    for (HNode **pp = &m->tab[hash(k) & (m->cap - 1)]; *pp; pp = &(*pp)->next)
        if (!strcmp((*pp)->key, k)) {
            HNode *n = *pp; void *v = n->val;
            *pp = n->next; free(n->key); free(n); m->size--;
            return v;
        }
    return NULL;
}

void hm_each(HMap *m, void (*fn)(const char *, void *, void *), void *ud) {
    for (size_t i = 0; i < m->cap; i++)
        for (HNode *n = m->tab[i]; n; n = n->next) fn(n->key, n->val, ud);
}

void hm_free(HMap *m, void (*free_val)(void *)) {
    for (size_t i = 0; i < m->cap; i++)
        for (HNode *n = m->tab[i], *nx; n; n = nx) {
            nx = n->next;
            if (free_val) free_val(n->val);
            free(n->key); free(n);
        }
    free(m->tab);
}
