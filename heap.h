#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct { long long at; size_t *ref; } HItem;
typedef struct { HItem *a; size_t n, cap; } Heap;

void heap_upsert(Heap *h, size_t *ref, long long at);
void heap_remove(Heap *h, size_t idx);
