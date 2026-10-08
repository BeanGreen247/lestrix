/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include <pty.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <fcntl.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(int argc,char**argv){
    int opost=atoi(argv[1]); size_t rbuf=(size_t)atol(argv[2])<<10, wbuf=(size_t)atol(argv[3])<<10; int linelen=atoi(argv[4]); size_t total=(size_t)256<<20;
    int m,s; struct winsize ws={50,200,0,0};
    openpty(&m,&s,NULL,NULL,&ws);
    struct termios t; tcgetattr(s,&t); if(!opost){t.c_oflag&=~(OPOST|ONLCR);} t.c_lflag&=~ECHO; tcsetattr(s,TCSANOW,&t);
    pid_t pid=fork();
    if(!pid){ close(m); char*b=malloc(wbuf); size_t n=0; while(n<wbuf){ for(int i=0;i<linelen&&n<wbuf;i++) b[n++]='a'+(i%26); if(n<wbuf) b[n++]='\n'; } size_t sent=0; while(sent<total){ ssize_t w=write(s,b,wbuf); if(w<=0)break; sent+=w;} close(s); _exit(0);}    
    close(s);
    char*rb=malloc(rbuf); size_t got=0; double t0=now(); ssize_t r; unsigned long reads=0;
    while((r=read(m,rb,rbuf))>0){ got+=r; reads++; if(got>=total) break; }
    double dt=now()-t0; kill(pid,9); waitpid(pid,0,0);
    printf("opost=%d rbuf=%zuK wbuf=%zuK line=%d: %.2f GB/s (%.2fs, avg read %.0f B)\n",opost,rbuf>>10,wbuf>>10,linelen,got/dt/1e9,dt,(double)got/reads);
    return 0;}
