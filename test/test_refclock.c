#define _POSIX_C_SOURCE 200809L
#include "../refclock.h"
#include "../service.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/shm.h>
#include <unistd.h>
static void pair(refclock *r,int64_t epoch) {
    refclock_frame(r,epoch,-120.*8000,1,0);
    refclock_frame(r,epoch+60,-60.*8000,1,0);
}
int main(void) {
    service_options o;service_defaults(&o);
    assert(service_setting(&o,"shm-unit","2")==0);
    assert(service_setting(&o,"shm-unit","2x")<0);
    assert(service_setting(&o,"correction","nan")<0);
    assert(service_setting(&o,"rate","8000.5")<0);
    assert(service_setting(&o,"sock","relative")<0);
    assert(service_setting(&o,"unknown","true")<0);
    char path[100];snprintf(path,sizeof(path),"/tmp/wwv-refclock-%ld.sock",(long)getpid());
    int fd=socket(AF_UNIX,SOCK_DGRAM,0);assert(fd>=0);
    struct sockaddr_un a;memset(&a,0,sizeof(a));a.sun_family=AF_UNIX;strcpy(a.sun_path,path);
    assert(bind(fd,(struct sockaddr*)&a,sizeof(a))==0);
    struct timeval timeout={1,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    refclock_options opt;refclock_defaults(&opt);strcpy(opt.sock,path);opt.correction=.125;
    refclock r;assert(!refclock_open(&r,&opt,8000));
    double real=wwv_now(0)-.02,mono=wwv_now(1)-.02;
    assert(refclock_capture(&r,0,160,real,mono)==0);
    int64_t epoch=1792600020;
    refclock_frame(&r,epoch,-120.*8000,1,0);assert(!refclock_tick(&r,0));
    refclock_frame(&r,epoch+60,-60.*8000,1,0);assert(refclock_tick(&r,0));
    struct wwv_sock_sample sample;assert(recv(fd,&sample,sizeof(sample),0)==sizeof(sample));
    assert(sample.magic==0x534f434b && sample.pulse==0 && sample.leap==0);
    assert(fabs(sample.tv.tv_sec+sample.tv.tv_usec*1e-6-real)<2e-6);
    assert(fabs(sample.offset+sample.tv.tv_sec+sample.tv.tv_usec*1e-6-(epoch+120+.125))<2e-6);
    assert(!refclock_tick(&r,0)); /* one sample per second */
    refclock_frame(&r,epoch+120,0,0,0);assert(!r.locked);
    pair(&r,epoch);assert(r.locked);
    refclock_frame(&r,epoch+120,0,1,1);assert(!r.locked && !r.pending);
    pair(&r,epoch);assert(r.locked);
    assert(refclock_capture(&r,160,160,real+10,mono+10)<0);assert(!r.locked);
    assert(!refclock_capture(&r,320,160,wwv_now(0)-.02,wwv_now(1)-.02));
    pair(&r,epoch);assert(r.locked);
    assert(refclock_capture(&r,480,160,wwv_now(0)-.04,wwv_now(1)-.04)==0);
    assert(refclock_capture(&r,640,160,wwv_now(0)-.3,wwv_now(1)-.3)==1);assert(!r.locked);
    refclock_close(&r);close(fd);unlink(path);
#ifdef __linux__
    refclock_defaults(&opt);
    opt.shm_unit=200;
    while(opt.shm_unit<=255 && shmget((key_t)(0x4e545030u+opt.shm_unit),0,0)>=0)opt.shm_unit++;
    assert(opt.shm_unit<=255); /* never touch a pre-existing segment */
    assert(!refclock_open(&r,&opt,8000));
    assert(!refclock_capture(&r,0,160,wwv_now(0)-.02,wwv_now(1)-.02));
    pair(&r,epoch);assert(refclock_tick(&r,0));
    assert(r.shm->mode==1 && r.shm->valid && !(r.shm->count&1));
    assert(r.shm->clockTimeStampSec==epoch+120);
    assert(r.shm->clockTimeStampNSec<1000000000 && r.shm->receiveTimeStampNSec<1000000000);
    int id=r.shmid;refclock_close(&r);assert(!shmctl(id,IPC_RMID,NULL));
#endif
    puts("refclock/config validation passed");return 0;
}
