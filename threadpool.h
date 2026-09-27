#pragma once
#include <pthread.h>

typedef struct Job { void (*fn)(void *); void *arg; struct Job *next; } Job;
typedef struct { pthread_t *th; int n; Job *head, *tail; pthread_mutex_t mu; pthread_cond_t cv; } TPool;

void tp_init(TPool *p, int nthreads);
void tp_submit(TPool *p, void (*fn)(void *), void *arg);
