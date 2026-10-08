/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <immintrin.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
typedef struct { char *a,*b; size_t n; int mode; int reps; double t; uint64_t sink; int cpu; } Job;
static void *run(void *p){
    Job *j=p; cpu_set_t s; CPU_ZERO(&s); CPU_SET(j->cpu,&s); sched_setaffinity(0,sizeof s,&s);
    double t0=now(); uint64_t acc=0;
    for(int r=0;r<j->reps;r++){
        if(j->mode==0) memcpy(j->b,j->a,j->n);
        else if(j->mode==1){ const __m256i *p=(const __m256i*)j->a; __m256i v=_mm256_setzero_si256(); for(size_t i=0;i<j->n/32;i++) v=_mm256_add_epi64(v,_mm256_load_si256(p+i)); acc+=_mm256_extract_epi64(v,0); }
        else if(j->mode==2) memset(j->b,r&0xff,j->n);
        else if(j->mode==3){ __m256i v=_mm256_set1_epi8(r); __m256i *p=(__m256i*)j->b; for(size_t i=0;i<j->n/32;i++) _mm256_stream_si256(p+i,v); _mm_sfence(); }
    }
    j->t=now()-t0; j->sink=acc; return NULL;
}
static double bench(int threads,size_t per,int mode,int reps){
    Job jobs[16]; pthread_t th[16];
    for(int i=0;i<threads;i++){ jobs[i].a=aligned_alloc(4096,per); jobs[i].b=aligned_alloc(4096,per); memset(jobs[i].a,1,per); memset(jobs[i].b,2,per); jobs[i].n=per; jobs[i].mode=mode; jobs[i].reps=reps; jobs[i].cpu=i*2%8; }
    double t0=now();
    for(int i=0;i<threads;i++) pthread_create(&th[i],0,run,&jobs[i]);
    for(int i=0;i<threads;i++) pthread_join(th[i],0);
    double dt=now()-t0; double bytes=(double)per*reps*threads*((mode==0)?2:1);
    for(int i=0;i<threads;i++){ free(jobs[i].a); free(jobs[i].b); }
    return bytes/dt/1e9;
}
int main(void){
    const char*names[]={"memcpy (R+W)","read (AVX2 sum)","memset write","stream write (NT)"};
    printf("size per thread / threads ->  GB/s (memcpy counts read+write traffic)\n");
    size_t sizes[]={16<<10,128<<10,2<<20,64<<20};
    const char*sn[]={"16KB(L1)","128KB(L2)","2MB(L3)","64MB(RAM)"};
    for(int m=0;m<4;m++){
        printf("%s\n",names[m]);
        for(int si=0;si<4;si++){
            printf("  %-10s",sn[si]);
            for(int th=1;th<=8;th*=2){ int reps=(int)((256<<20)/sizes[si]); if(reps<2)reps=2; double g=bench(th,sizes[si],m,reps*2); printf(" %dT %6.1f |",th,g); }
            printf("\n");
        }
    }
    return 0;
}
