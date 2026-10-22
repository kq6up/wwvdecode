#ifndef WWV_SERVICE_H
#define WWV_SERVICE_H
#include "refclock.h"
typedef struct {
    char device[512], pidfile[512], logfile[512];
    unsigned rate;
    int daemonize, verbose, auto_offset, manual_offset;
    double offset, harm_reject, min_snr;
    refclock_options output;
} service_options;
void service_defaults(service_options *o);
int service_setting(service_options *o,const char *key,const char *value);
int service_config(service_options *o,const char *path);
/* 1 in original parent after child reports startup, 0 in service, -1 error. */
int service_start(const service_options *o);
void service_ready(int status);
void service_stop(void);
#endif
