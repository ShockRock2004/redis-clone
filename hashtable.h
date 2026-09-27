#pragma once
#include <stddef.h>

typedef struct HNode { struct HNode *next; char *key; void *val; } HNode;
typedef struct { HNode **tab; size_t cap, size; } HMap;

void hm_init(HMap *m);
void **hm_get(HMap *m, const char *key);
void hm_set(HMap *m, const char *key, void *val);
void *hm_del(HMap *m, const char *key);
void hm_each(HMap *m, void (*fn)(const char *, void *, void *), void *ud);
void hm_free(HMap *m, void (*free_val)(void *));
