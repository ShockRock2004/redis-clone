#include <stdlib.h>
#include "heap.h"

static void place(Heap *h, size_t i) { *h->a[i].ref = i; }
static void swap(Heap *h, size_t i, size_t j) {
    HItem t = h->a[i]; h->a[i] = h->a[j]; h->a[j] = t;
    place(h, i); place(h, j);
}

static void fix(Heap *h, size_t i) {
    while (i > 0 && h->a[(i - 1) / 2].at > h->a[i].at) { swap(h, i, (i - 1) / 2); i = (i - 1) / 2; }
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < h->n && h->a[l].at < h->a[m].at) m = l;
        if (r < h->n && h->a[r].at < h->a[m].at) m = r;
        if (m == i) break;
        swap(h, i, m); i = m;
    }
}

void heap_upsert(Heap *h, size_t *ref, long long at) {
    size_t i = *ref;
    if (i == SIZE_MAX) {
        if (h->n == h->cap) {
            h->cap = h->cap ? h->cap * 2 : 16;
            h->a = realloc(h->a, h->cap * sizeof(HItem));
        }
        i = h->n++;
        h->a[i].ref = ref;
    }
    h->a[i].at = at;
    place(h, i);
    fix(h, i);
}

void heap_remove(Heap *h, size_t i) {
    *h->a[i].ref = SIZE_MAX;
    if (i < --h->n) { h->a[i] = h->a[h->n]; place(h, i); fix(h, i); }
}
