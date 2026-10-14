/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>

#include "../src/bench.h"

int main(void) { return sd_benchmark(stdout, getenv("BENCH_MB") ? atof(getenv("BENCH_MB")) : 8.0); }
