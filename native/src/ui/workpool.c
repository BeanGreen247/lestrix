#include "workpool.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

static struct {
    pthread_mutex_t m;
    pthread_cond_t wake, done;
    void (*fn)(void *, int);
    void *arg;
    int n;
    atomic_int next, remaining;
    unsigned generation;
    int workers, started, configured;
} P = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, NULL, 0, 0, 0, 0, 0, 0, 0};

void wp_configure(int workers) { P.configured = workers; }

int wp_workers(void) {
    int w = P.configured;
    if (w < 0) w = 0;   /* off unless asked for: at 0.5 ms a frame the hand-over costs more than building rows in parallel saves (measured) */
    return w > 8 ? 8 : w;
}

static void work(void) {
    for (;;) {
        int i = atomic_fetch_add(&P.next, 1);
        if (i >= P.n) return;
        P.fn(P.arg, i);
        if (atomic_fetch_sub(&P.remaining, 1) == 1) {
            pthread_mutex_lock(&P.m);
            pthread_cond_broadcast(&P.done);
            pthread_mutex_unlock(&P.m);
        }
    }
}

static void *worker_main(void *unused) {
    (void)unused;
    unsigned seen = 0;
    for (;;) {
        pthread_mutex_lock(&P.m);
        while (P.generation == seen) pthread_cond_wait(&P.wake, &P.m);
        seen = P.generation;
        pthread_mutex_unlock(&P.m);
        work();
    }
    return NULL;
}

void wp_run(void (*fn)(void *, int), void *arg, int n) {
    int w = wp_workers();
    if (w <= 0 || n < 2) { for (int i = 0; i < n; i++) fn(arg, i); return; }
    pthread_mutex_lock(&P.m);
    while (P.started < w) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 512 * 1024);
        if (pthread_create(&th, NULL, worker_main, NULL) != 0) { pthread_attr_destroy(&at); break; }
        pthread_attr_destroy(&at);
        P.started++;
    }
    P.fn = fn; P.arg = arg; P.n = n;
    atomic_store(&P.next, 0); atomic_store(&P.remaining, n);
    P.generation++;
    pthread_cond_broadcast(&P.wake);
    pthread_mutex_unlock(&P.m);
    work();
    pthread_mutex_lock(&P.m);
    while (atomic_load(&P.remaining) > 0) pthread_cond_wait(&P.done, &P.m);
    pthread_mutex_unlock(&P.m);
}
