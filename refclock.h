#ifndef WWV_REFCLOCK_H
#define WWV_REFCLOCK_H
#include <stdint.h>
#include <stddef.h>
#include <sys/time.h>
/* Native ABI shared with chrony and ntpd. No clock-setting operations. */
struct wwv_shm_time {
    int mode; volatile int count;
    time_t clockTimeStampSec; int clockTimeStampUSec;
    time_t receiveTimeStampSec; int receiveTimeStampUSec;
    int leap, precision, nsamples; volatile int valid;
    unsigned clockTimeStampNSec, receiveTimeStampNSec;
    int dummy[8];
};
struct wwv_sock_sample { struct timeval tv; double offset; int pulse, leap, pad, magic; };
typedef struct {
    int shm_unit, precision;
    char sock[104];
    double correction;
} refclock_options;
typedef struct {
    refclock_options opt;
    struct wwv_shm_time *shm;
    int shmid, sockfd, lockfd, have_capture, pending, locked;
    unsigned rate;
    uint64_t block_index, end_index, sent, attempts;
    double real, mono, end_mono, real_minus_mono;
    int64_t previous_epoch, epoch, last_tick;
    double previous_boundary, boundary;
} refclock;
double wwv_now(int monotonic);
void refclock_defaults(refclock_options *o);
int refclock_open(refclock *r, const refclock_options *o, unsigned rate);
void refclock_close(refclock *r);
void refclock_reset(refclock *r, const char *reason);
int refclock_capture(refclock *r, uint64_t index, size_t n, double real, double mono);
void refclock_frame(refclock *r, int64_t epoch, double boundary, int valid, int leap_warning);
int refclock_tick(refclock *r, double index);
#endif
