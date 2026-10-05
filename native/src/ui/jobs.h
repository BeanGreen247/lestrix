/* jobs.h - run blocking work (host probes, SFTP calls) on a few worker threads and finish it on the UI thread. */
#ifndef SD_JOBS_H
#define SD_JOBS_H
#include <stdbool.h>

typedef void *(*JobWork)(void *arg);                    /* worker thread; returns a result */
typedef void (*JobDone)(void *arg, void *result);       /* UI thread, from jobs_pump */

void jobs_init(void (*wake)(void));                     /* wake: any thread, "call jobs_pump soon" */
void jobs_shutdown(void);
void jobs_run(JobWork work, JobDone done, void *arg);
bool jobs_pump(void);                                   /* UI thread: run finished jobs' callbacks; true if any ran */
int jobs_pending(void);
#endif
