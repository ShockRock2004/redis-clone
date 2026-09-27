#include <stdlib.h>
#include "threadpool.h"

static void *worker(void *arg) {
    TPool *p = arg;
    for (;;) {
        pthread_mutex_lock(&p->mu);
        while (!p->head) pthread_cond_wait(&p->cv, &p->mu);
        Job *j = p->head;
        p->head = j->next;
        if (!p->head) p->tail = NULL;
        pthread_mutex_unlock(&p->mu);
        j->fn(j->arg);
        free(j);
    }
    return NULL;
}

void tp_init(TPool *p, int n) {
    p->n = n; p->head = p->tail = NULL;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    p->th = malloc(n * sizeof(pthread_t));
    for (int i = 0; i < n; i++) pthread_create(&p->th[i], NULL, worker, p);
}

void tp_submit(TPool *p, void (*fn)(void *), void *arg) {
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->next = NULL;
    pthread_mutex_lock(&p->mu);
    if (p->tail) p->tail->next = j; else p->head = j;
    p->tail = j;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
}
