#include "jobs.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#define NWORKERS 6

typedef struct Job { JobWork work; JobDone done; void *arg, *result; struct Job *next; } Job;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static Job *q_head, *q_tail, *done_head, *done_tail;
static pthread_t workers[NWORKERS];
static int started, quitting;
static atomic_int pending;
static void (*wake_fn)(void);

static void *worker(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!q_head && !quitting) pthread_cond_wait(&cv, &mu);
        if (quitting && !q_head) { pthread_mutex_unlock(&mu); return NULL; }
        Job *j = q_head;
        q_head = j->next;
        if (!q_head) q_tail = NULL;
        pthread_mutex_unlock(&mu);
        j->result = j->work(j->arg);
        j->next = NULL;
        pthread_mutex_lock(&mu);
        if (done_tail) done_tail->next = j; else done_head = j;
        done_tail = j;
        pthread_mutex_unlock(&mu);
        if (wake_fn) wake_fn();
    }
}

void jobs_init(void (*wake)(void)) {
    wake_fn = wake;
    for (int i = 0; i < NWORKERS; i++) if (pthread_create(&workers[i], NULL, worker, NULL) == 0) started++;
}

void jobs_shutdown(void) {
    pthread_mutex_lock(&mu);
    quitting = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    /* workers may be inside a long blocking call: do not wait for them at exit */
    for (int i = 0; i < started; i++) pthread_detach(workers[i]);
}

void jobs_run(JobWork work, JobDone done, void *arg) {
    Job *j = calloc(1, sizeof *j);
    if (!j) return;
    j->work = work; j->done = done; j->arg = arg;
    atomic_fetch_add(&pending, 1);
    pthread_mutex_lock(&mu);
    if (q_tail) q_tail->next = j; else q_head = j;
    q_tail = j;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}

int jobs_pending(void) { return atomic_load(&pending); }

bool jobs_pump(void) {
    bool any = false;
    for (;;) {
        pthread_mutex_lock(&mu);
        Job *j = done_head;
        if (j) { done_head = j->next; if (!done_head) done_tail = NULL; }
        pthread_mutex_unlock(&mu);
        if (!j) break;
        if (j->done) j->done(j->arg, j->result);
        atomic_fetch_sub(&pending, 1);
        free(j);
        any = true;
    }
    return any;
}
