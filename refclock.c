#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "refclock.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

double wwv_now(int monotonic) {
    struct timespec t;
    if (clock_gettime(monotonic ? CLOCK_MONOTONIC : CLOCK_REALTIME, &t)) return NAN;
    return (double)t.tv_sec + t.tv_nsec * 1e-9;
}
void refclock_defaults(refclock_options *o) {
    memset(o,0,sizeof(*o)); o->shm_unit=-1; o->precision=-5;
}
void refclock_reset(refclock *r, const char *reason) {
    if (r->locked || r->pending) fprintf(stderr,"refclock: UTC lock cleared (%s)\n",reason);
    r->locked=r->pending=0; r->last_tick=INT64_MIN;
    if(r->shm) __atomic_store_n(&r->shm->valid,0,__ATOMIC_SEQ_CST);
}
int refclock_open(refclock *r, const refclock_options *o, unsigned rate) {
    memset(r,0,sizeof(*r)); r->opt=*o; r->rate=rate;
    r->shmid=r->sockfd=r->lockfd=-1; r->last_tick=INT64_MIN;
    if(o->shm_unit>=0) {
        char path[128];
        snprintf(path,sizeof(path),"/tmp/wwvdecode-shm-%lu-%d.lock",(unsigned long)geteuid(),o->shm_unit);
        r->lockfd=open(path,O_RDWR|O_CREAT|O_NOFOLLOW,0600);
        struct stat st;
        if(r->lockfd<0 || fstat(r->lockfd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() ||
           (st.st_mode&0022) || flock(r->lockfd,LOCK_EX|LOCK_NB)) {
            fprintf(stderr,"refclock: cannot exclusively lock SHM unit %d\n",o->shm_unit); goto fail;
        }
        key_t key=(key_t)(0x4e545030u+(unsigned)o->shm_unit);
        int created=0;
        r->shmid=shmget(key,sizeof(*r->shm),IPC_CREAT|IPC_EXCL|0600);
        if(r->shmid>=0) created=1;
        else if(errno==EEXIST) r->shmid=shmget(key,sizeof(*r->shm),0600);
        struct shmid_ds ds;
        if(r->shmid<0 || shmctl(r->shmid,IPC_STAT,&ds) || ds.shm_segsz!=sizeof(*r->shm) ||
           ds.shm_perm.uid!=geteuid() || (ds.shm_perm.mode&0022)) {
            fprintf(stderr,"refclock: SHM must have compatible size, same owner and no group/world writes\n"); goto fail;
        }
        r->shm=shmat(r->shmid,NULL,0);
        if(r->shm==(void*)-1) { r->shm=NULL; perror("shmat"); goto fail; }
        if(created) memset(r->shm,0,sizeof(*r->shm));
        __atomic_store_n(&r->shm->valid,0,__ATOMIC_SEQ_CST);
        fprintf(stderr,"refclock: SHM unit %d, private permissions 0600\n",o->shm_unit);
    }
    return 0;
fail:
    refclock_close(r); return -1;
}
void refclock_close(refclock *r) {
    if(r->shm) { __atomic_store_n(&r->shm->valid,0,__ATOMIC_SEQ_CST); shmdt(r->shm); r->shm=NULL; }
    if(r->sockfd>=0) { close(r->sockfd); r->sockfd=-1; }
    if(r->lockfd>=0) { close(r->lockfd); r->lockfd=-1; }
}
int refclock_capture(refclock *r,uint64_t index,size_t n,double real,double mono) {
    double now=wwv_now(0), mn=wwv_now(1), duration=(double)n/r->rate;
    if(!isfinite(real)||!isfinite(mono)||duration<=0 || duration>1 ||
       now-real-duration < -.1 || now-real>2 || fabs((now-real)-(mn-mono))>.05) {
        refclock_reset(r,"invalid or stale capture timestamp"); r->have_capture=0; return -1;
    }
    int gap=r->have_capture && (index!=r->end_index || fabs(mono-r->end_mono)>.05 ||
                               fabs((real-mono)-r->real_minus_mono)>.02);
    if(gap) refclock_reset(r,"capture discontinuity or system clock step");
    r->block_index=index; r->end_index=index+n; r->real=real; r->mono=mono;
    r->end_mono=mono+duration; r->real_minus_mono=real-mono; r->have_capture=1;
    return gap ? 1 : 0;
}
void refclock_frame(refclock *r,int64_t epoch,double boundary,int valid,int leap_warning) {
    /* A WWV warning does not encode leap direction. Suspend through it rather
       than assume a positive leap, or extrapolate over the event. */
    if(!valid || leap_warning || !r->have_capture) { refclock_reset(r,"invalid frame or leap warning"); return; }
    int agrees=r->pending && epoch==r->previous_epoch+60 &&
               fabs((boundary-r->previous_boundary)/r->rate-60)<.04;
    if(r->locked && !agrees) refclock_reset(r,"inconsistent UTC minute");
    r->locked=agrees; r->pending=1; r->previous_epoch=epoch; r->previous_boundary=boundary;
    r->epoch=epoch; r->boundary=boundary;
    if(agrees) fprintf(stderr,"refclock: confirmed UTC anchor %lld; publishing fresh ticks\n",(long long)epoch);
}
static void split_time(double value,time_t *sec,unsigned *ns) {
    double whole=floor(value); long nano=lround((value-whole)*1e9);
    if(nano==1000000000L) { whole+=1; nano=0; }
    *sec=(time_t)whole; *ns=(unsigned)nano;
}
static int sock_send(refclock *r,double received,double utc) {
    if(r->sockfd<0) {
        struct sockaddr_un a; memset(&a,0,sizeof(a)); a.sun_family=AF_UNIX;
        memcpy(a.sun_path,r->opt.sock,strlen(r->opt.sock)+1);
        r->sockfd=socket(AF_UNIX,SOCK_DGRAM,0);
        if(r->sockfd<0) return -1;
        int flags=fcntl(r->sockfd,F_GETFL); fcntl(r->sockfd,F_SETFL,flags|O_NONBLOCK);
        if(connect(r->sockfd,(struct sockaddr*)&a,sizeof(a))) { close(r->sockfd);r->sockfd=-1;return -1; }
    }
    struct wwv_sock_sample s; memset(&s,0,sizeof(s));
    unsigned ns; time_t sec; split_time(received,&sec,&ns);
    s.tv.tv_sec=sec;s.tv.tv_usec=ns/1000;
    s.offset=utc-((double)sec+s.tv.tv_usec*1e-6);s.magic=0x534f434b;
    if(send(r->sockfd,&s,sizeof(s),0)!=(ssize_t)sizeof(s)) { close(r->sockfd);r->sockfd=-1;return -1; }
    return 0;
}
int refclock_tick(refclock *r,double index) {
    if(!r->locked || !r->have_capture) return 0;
    double age=(index-r->boundary)/r->rate;
    if(age<60 || age>125) { refclock_reset(r,"UTC anchor expired"); return 0; }
    int64_t second=(int64_t)llround(age);
    if(fabs(age-second)>.025 || r->epoch+second<=r->last_tick) return 0;
    double delta=(index-r->block_index)/r->rate;
    double received=r->real+delta, measured_mono=r->mono+delta;
    double now=wwv_now(0), mn=wwv_now(1);
    if(mn-measured_mono<-.1 || mn-measured_mono>2 || fabs((now-mn)-r->real_minus_mono)>.02) {
        refclock_reset(r,"late sample or system clock step"); return 0;
    }
    double utc=(double)(r->epoch+second)+r->opt.correction;
    int success=0; r->attempts++;
    if(r->shm) {
        struct wwv_shm_time *s=r->shm;
        __atomic_store_n(&s->valid,0,__ATOMIC_SEQ_CST);
        __atomic_add_fetch(&s->count,1,__ATOMIC_SEQ_CST);
        s->mode=1;s->leap=0;s->precision=r->opt.precision;s->nsamples=1;
        split_time(utc,&s->clockTimeStampSec,&s->clockTimeStampNSec);
        split_time(received,&s->receiveTimeStampSec,&s->receiveTimeStampNSec);
        s->clockTimeStampUSec=s->clockTimeStampNSec/1000;
        s->receiveTimeStampUSec=s->receiveTimeStampNSec/1000;
        __atomic_add_fetch(&s->count,1,__ATOMIC_SEQ_CST);
        __atomic_store_n(&s->valid,1,__ATOMIC_SEQ_CST);success=1;
    }
    if(r->opt.sock[0]) {
        if(!sock_send(r,received,utc)) success=1;
        else if((r->attempts-1)%30==0) fprintf(stderr,"refclock: SOCK unavailable: %s (will retry)\n",strerror(errno));
    }
    r->last_tick=r->epoch+second;
    if(success) r->sent++;
    return success;
}
