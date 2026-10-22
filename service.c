#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "service.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static int pidfd=-1, readyfd=-1;
static char pidpath[512];
void service_defaults(service_options *o) {
    memset(o,0,sizeof(*o));strcpy(o->device,"default");o->rate=8000;
    o->harm_reject=.5;o->min_snr=3;refclock_defaults(&o->output);
}
static int copy_path(char *out,size_t n,const char *s) {
    if(!*s||strlen(s)>=n) return -1;
    strcpy(out,s);return 0;
}
static int number(const char *s,double lo,double hi,double *v) {
    char *end;errno=0;*v=strtod(s,&end);
    return errno || end==s || *end || !isfinite(*v) || *v<lo || *v>hi ? -1:0;
}
static int boolean(const char *s,int *v) {
    if(!strcmp(s,"true")||!strcmp(s,"1")){*v=1;return 0;}
    if(!strcmp(s,"false")||!strcmp(s,"0")){*v=0;return 0;}
    return -1;
}
int service_setting(service_options *o,const char *key,const char *v) {
    double x;
    if(!strcmp(key,"device")) return copy_path(o->device,sizeof(o->device),v);
    if(!strcmp(key,"pidfile")) return v[0]=='/' ? copy_path(o->pidfile,sizeof(o->pidfile),v):-1;
    if(!strcmp(key,"logfile")) return v[0]=='/' ? copy_path(o->logfile,sizeof(o->logfile),v):-1;
    if(!strcmp(key,"sock")) return v[0]=='/' ? copy_path(o->output.sock,sizeof(o->output.sock),v):-1;
    if(!strcmp(key,"daemon")) return boolean(v,&o->daemonize);
    if(!strcmp(key,"verbose")) return boolean(v,&o->verbose);
    if(!strcmp(key,"auto-offset")) return boolean(v,&o->auto_offset);
    if(!strcmp(key,"rate")) {if(number(v,4000,192000,&x)||x!=floor(x))return -1;o->rate=(unsigned)x;return 0;}
    if(!strcmp(key,"shm-unit")) {if(number(v,-1,255,&x)||x!=floor(x))return -1;o->output.shm_unit=(int)x;return 0;}
    if(!strcmp(key,"precision")) {if(number(v,-30,0,&x)||x!=floor(x))return -1;o->output.precision=(int)x;return 0;}
    if(!strcmp(key,"correction")) return number(v,-10,10,&o->output.correction);
    if(!strcmp(key,"offset")) {int r=number(v,-3000,3000,&o->offset);if(!r)o->manual_offset=1;return r;}
    if(!strcmp(key,"harm-reject")) return number(v,.01,100,&o->harm_reject);
    if(!strcmp(key,"min-snr")) return number(v,0,100,&o->min_snr);
    return -1;
}
static char *trim(char *s) {
    while(isspace((unsigned char)*s))s++;
    char *end=s+strlen(s);while(end>s&&isspace((unsigned char)end[-1]))*--end=0;return s;
}
int service_config(service_options *o,const char *path) {
    FILE *f=fopen(path,"r");if(!f){perror(path);return -1;}
    char line[2048];unsigned n=0;int rc=0;
    while(fgets(line,sizeof(line),f)) {
        ++n;char *s=trim(line),*eq;
        if(!*s||*s=='#')continue;
        eq=strchr(s,'=');if(!eq){rc=-1;break;}*eq++=0;
        if(service_setting(o,trim(s),trim(eq))) {rc=-1;break;}
    }
    if(ferror(f))rc=-1;
    if(rc)fprintf(stderr,"%s:%u: unknown key or invalid value (expected key=value)\n",path,n);
    fclose(f);return rc;
}
static int lock_pid(const char *path) {
    pidfd=open(path,O_RDWR|O_CREAT|O_NOFOLLOW,0600);
    struct stat st;
    if(pidfd<0||fstat(pidfd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||
       (st.st_mode&0022)||flock(pidfd,LOCK_EX|LOCK_NB)) {
        fprintf(stderr,"cannot lock pidfile %s: %s\n",path,strerror(errno));return -1;
    }
    strcpy(pidpath,path);return 0;
}
int service_start(const service_options *o) {
    if(o->pidfile[0]&&lock_pid(o->pidfile))return -1;
    if(o->daemonize) {
        int p[2];if(pipe(p))return -1;
        pid_t child=fork();if(child<0){close(p[0]);close(p[1]);return -1;}
        if(child) {
            close(p[1]);if(pidfd>=0){close(pidfd);pidfd=-1;}
            unsigned char status=1;ssize_t n;
            do {n=read(p[0],&status,1);}while(n<0&&errno==EINTR);
            close(p[0]);waitpid(child,NULL,0);return n==1&&status==0 ? 1:-1;
        }
        close(p[0]);readyfd=p[1];
        if(setsid()<0){service_ready(1);_exit(1);}
        child=fork();if(child<0){service_ready(1);_exit(1);}if(child)_exit(0);
        if(chdir("/")){service_ready(1);_exit(1);}
        umask(027);
        int nullfd=open("/dev/null",O_RDWR);
        int logfd=open(o->logfile,O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0640);
        if(nullfd<0||logfd<0){service_ready(1);_exit(1);}
        if(dup2(nullfd,STDIN_FILENO)<0||dup2(logfd,STDOUT_FILENO)<0||dup2(logfd,STDERR_FILENO)<0){service_ready(1);_exit(1);}
        if(nullfd>2)close(nullfd);
        if(logfd>2)close(logfd);
    } else if(o->logfile[0]) {
        int fd=open(o->logfile,O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW,0640);
        if(fd<0){perror(o->logfile);return -1;}
        if(dup2(fd,STDOUT_FILENO)<0||dup2(fd,STDERR_FILENO)<0){close(fd);return -1;}
        if(fd>2)close(fd);
    }
    setvbuf(stdout,NULL,_IOLBF,0);
    if(pidfd>=0) {
        char buf[64];int n=snprintf(buf,sizeof(buf),"%ld\n",(long)getpid());
        if(ftruncate(pidfd,0)||pwrite(pidfd,buf,(size_t)n,0)!=n){service_ready(1);return -1;}
    }
    return 0;
}
void service_ready(int status) {
    if(readyfd>=0){unsigned char s=(unsigned char)status;ssize_t n=write(readyfd,&s,1);(void)n;close(readyfd);readyfd=-1;}
}
void service_stop(void) {
    service_ready(1);
    /* Unlink while still holding the lock; caller must not replace this path. */
    if(pidfd>=0){unlink(pidpath);close(pidfd);pidfd=-1;}
}
