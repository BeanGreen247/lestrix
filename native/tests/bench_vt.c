/* Throughput of the terminal core. BENCH_MB sets the size per test (default 8). */
#include <stdlib.h>

#include "../src/bench.h"

int main(void) { return sd_benchmark(stdout, getenv("BENCH_MB") ? atof(getenv("BENCH_MB")) : 8.0); }
