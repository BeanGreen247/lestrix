/* workpool.h - a few helper threads for jobs that split into independent pieces (building the rows of a frame). The caller takes part
 * in the work and the call returns when every piece is done. */
#ifndef SD_WORKPOOL_H
#define SD_WORKPOOL_H

/* helper threads to use: 0 or -1 = none (the default), N = N helpers. Call before the first wp_run. */
void wp_configure(int workers);
int wp_workers(void);
/* runs fn(arg, 0) ... fn(arg, n-1), each exactly once, spread over the helpers and the calling thread */
void wp_run(void (*fn)(void *arg, int index), void *arg, int n);

#endif
