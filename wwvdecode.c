/*
 * wwvdecode.c - decode the WWV 100 Hz time code from receiver audio.
 *
 * Changes from wwvdecode_3.c:
 *
 *   1. File input.  -i/--input FILE reads a recording instead of ALSA.
 *      Raw S16_LE mono is assumed unless the file starts with a RIFF/WAVE
 *      header, which is parsed (8/16/24/32-bit PCM or float, mono or
 *      multi-channel; extra channels are averaged).
 *
 *   2. Offline (two-pass) processing for files.  The whole recording is in
 *      memory, so the tone offset is estimated from all of it, and the
 *      minute phase is found by an exhaustive matched-filter search rather
 *      than by waiting for a marker to fall out of a streaming detector.
 *
 *   3. Harmonic gating on the 1000/1500 Hz detectors.  Transmitter and
 *      receiver distortion of the 500 Hz program tone puts energy at
 *      exactly 1000 and 1500 Hz for 45 s of every other minute, which the
 *      old code happily accepted as second ticks and minute/hour markers.
 *      A 1000 Hz event is now rejected unless it dominates the 500 Hz
 *      level (likewise 1500 Hz vs 500 Hz, and 1200/1800 Hz vs 600 Hz).
 *
 *   4. Power-domain symbol decisions.  The 100 Hz windows integrate
 *      |z|^2 from a coherent 5 ms correlation instead of a slow envelope,
 *      and each second gets a measured SNR so unreliable bits are marked
 *      '?' instead of guessed.
 *
 *   5. --analyze prints a link budget: tone offset, audio passband
 *      profile, 100 Hz code E/N0 per symbol type, program tone segments,
 *      and validated minute-marker candidates.  If a recording cannot be
 *      decoded, this says why.
 *
 * Build:
 *   with ALSA:    cc -O2 -o wwvdecode wwvdecode.c -lasound -lm
 *   without:      cc -O2 -DWWV_NO_ALSA -o wwvdecode wwvdecode.c -lm
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef WWV_NO_ALSA
#include <alsa/asoundlib.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEFAULT_RATE 8000u
#define DEFAULT_DEVICE "default"
#define BLOCK_FRAMES 1024
#define MAX_SECONDS 60

/* Coherent correlation block for the offline 100 Hz envelope, in seconds.
   5 ms is the output time grid. Complex samples are Hann-smoothed over
   80 ms before power detection, preserving edges while rejecting noise. */
#define CODE_BLOCK 0.005

/* Measurement block for the analysis report.  This has to be much narrower
   than CODE_BLOCK: a 5 ms correlation is 200 Hz wide, so a "100 Hz" reading
   taken with it is mostly the 400-700 Hz program-tone energy leaking in.
   0.25 s gives about 4 Hz, narrow enough to separate the code line from its
   neighbours and still shorter than a marker pulse. */
#define PROBE_BLOCK 0.250

/* WWV pulse geometry, seconds after the second boundary. */
#define PULSE_START 0.030
#define PULSE_ZERO  0.200
#define PULSE_ONE   0.500
#define PULSE_MARK  0.800

typedef enum {
    SYM_UNKNOWN = -1,
    SYM_ZERO = 0,
    SYM_ONE = 1,
    SYM_MARK = 2,
    SYM_HOLE = 3
} symbol_t;

/* ------------------------------------------------------------------ */
/* PCM input                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int16_t *s;
    size_t n;
    unsigned rate;
} pcm_t;

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static void pcm_free(pcm_t *p) { free(p->s); p->s = NULL; p->n = 0; }

/* Decode interleaved PCM of the given format into mono int16. */
static int pcm_convert(const unsigned char *data, size_t bytes,
                       unsigned fmt, unsigned bits, unsigned ch, pcm_t *out) {
    if (fmt != 1 && fmt != 3) return -1;
    if (fmt == 3 && bits != 32) return -1;
    unsigned bps = bits / 8;
    if (bps == 0 || ch == 0) return -1;
    size_t frames = bytes / (bps * ch);
    if (frames == 0) return -1;

    int16_t *s = malloc(frames * sizeof(*s));
    if (!s) return -1;

    for (size_t i = 0; i < frames; ++i) {
        double acc = 0.0;
        for (unsigned c = 0; c < ch; ++c) {
            const unsigned char *p = data + (i * ch + c) * bps;
            double v = 0.0;
            if (fmt == 3 && bits == 32) {           /* IEEE float */
                uint32_t u = rd32(p);
                float f;
                memcpy(&f, &u, sizeof(f));
                v = (double)f * 32768.0;
            } else if (bits == 8) {                 /* WAV 8-bit is unsigned */
                v = ((double)p[0] - 128.0) * 256.0;
            } else if (bits == 16) {
                v = (double)(int16_t)rd16(p);
            } else if (bits == 24) {
                int32_t u = (int32_t)((uint32_t)p[0] << 8 |
                                      (uint32_t)p[1] << 16 |
                                      (uint32_t)p[2] << 24);
                v = (double)(u >> 8) / 256.0;
            } else if (bits == 32) {
                v = (double)(int32_t)rd32(p) / 65536.0;
            } else {
                free(s);
                return -1;
            }
            acc += v;
        }
        acc /= (double)ch;
        if (acc > 32767.0) acc = 32767.0;
        if (acc < -32768.0) acc = -32768.0;
        s[i] = (int16_t)lrint(acc);
    }
    out->s = s;
    out->n = frames;
    return 0;
}

static int pcm_load(const char *path, unsigned rate_hint, pcm_t *out) {
    FILE *f = strcmp(path, "-") ? fopen(path, "rb") : stdin;
    if (!f) { perror(path); return -1; }

    size_t cap = 1u << 20, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) { if (f != stdin) fclose(f); return -1; }
    for (;;) {
        if (len == cap) {
            cap *= 2;
            unsigned char *nb = realloc(buf, cap);
            if (!nb) { free(buf); if (f != stdin) fclose(f); return -1; }
            buf = nb;
        }
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) break;
    }
    if (f != stdin) fclose(f);
    if (len == 0) { free(buf); fprintf(stderr, "%s: empty\n", path); return -1; }

    out->rate = rate_hint;

    /* RIFF/WAVE? */
    if (len > 44 && !memcmp(buf, "RIFF", 4) && !memcmp(buf + 8, "WAVE", 4)) {
        unsigned fmt = 1, bits = 16, ch = 1;
        unsigned rate = rate_hint;
        size_t off = 12;
        const unsigned char *data = NULL;
        size_t dlen = 0;
        int have_fmt = 0;
        while (off + 8 <= len) {
            uint32_t csz = rd32(buf + off + 4);
            const unsigned char *cp = buf + off + 8;
            size_t avail = len - (off + 8);
            if (csz > avail) csz = (uint32_t)avail;
            if (!memcmp(buf + off, "fmt ", 4) && csz >= 16) {
                fmt = rd16(cp);
                ch = rd16(cp + 2);
                rate = rd32(cp + 4);
                bits = rd16(cp + 14);
                if (fmt == 0xFFFE && csz >= 40) fmt = rd16(cp + 24);
                have_fmt = 1;
            } else if (!memcmp(buf + off, "data", 4)) {
                data = cp;
                dlen = csz;
            }
            off += 8 + csz + (csz & 1);
        }
        if (!have_fmt || !data) {
            fprintf(stderr, "%s: WAVE file with no fmt/data chunk\n", path);
            free(buf);
            return -1;
        }
        if (rate < 4000 || rate > 192000) {
            fprintf(stderr,"%s: unsupported WAVE sample rate %u\n",path,rate);
            free(buf); return -1;
        }
        out->rate = rate;
        int rc = pcm_convert(data, dlen, fmt, bits, ch, out);
        free(buf);
        if (rc < 0) {
            fprintf(stderr, "%s: unsupported WAVE format %u, %u-bit, %u ch\n",
                    path, fmt, bits, ch);
            return -1;
        }
        fprintf(stderr, "input: %s WAVE %u-bit %u ch %u Hz, %zu frames (%.1f s)\n",
                path, bits, ch, out->rate, out->n, (double)out->n / out->rate);
        return 0;
    }

    /* Headerless: S16_LE mono at the assumed rate. */
    if (pcm_convert(buf, len & ~(size_t)1, 1, 16, 1, out) < 0) {
        free(buf);
        return -1;
    }
    free(buf);
    fprintf(stderr, "input: %s raw S16_LE mono %u Hz, %zu frames (%.1f s)\n",
            path, out->rate, out->n, (double)out->n / out->rate);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Narrowband measurement helpers                                     */
/* ------------------------------------------------------------------ */

/* Mean power at freq, measured with coherent blocks of blk_sec.  This is
   the same estimator the decoder uses, so levels are comparable. */
static double band_power(const int16_t *s, size_t n, unsigned rate,
                         double freq, double blk_sec) {
    size_t L = (size_t)llround(blk_sec * (double)rate);
    if (L < 4) L = 4;
    if (n < L) return 0.0;
    double w = 2.0 * M_PI * freq / (double)rate;
    double cd = cos(w), sd = sin(w);
    double c = 1.0, sn = 0.0;
    double acc = 0.0;
    size_t nb = 0;
    for (size_t b = 0; b + L <= n; b += L) {
        double re = 0.0, im = 0.0;
        for (size_t i = 0; i < L; ++i) {
            double x = (double)s[b + i] / 32768.0;
            re += x * c;
            im += x * sn;
            double nc = c * cd - sn * sd;
            double ns = sn * cd + c * sd;
            c = nc; sn = ns;
        }
        double r = hypot(c, sn);
        if (r > 0.0) { c /= r; sn /= r; }
        re /= (double)L; im /= (double)L;
        acc += 4.0 * (re * re + im * im);
        nb++;
    }
    return nb ? acc / (double)nb : 0.0;
}

/* ------------------------------------------------------------------ */
/* 100 Hz code envelope (offline)                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    double *pw;     /* narrowband power on a 5 ms time grid */
    double *prefix; /* prefix sums for constant-time window integration */
    double *ci, *cq; /* complex baseband prefix sums for coherent symbol windows */
    size_t n;
    double sps;     /* blocks per second */
} cenv_t;

static int cenv_build(cenv_t *e, const int16_t *s, size_t n, unsigned rate,
                      double freq) {
    size_t L = (size_t)llround(CODE_BLOCK * (double)rate);
    if (L < 4) L = 4;
    e->n = n / L;
    e->sps = (double)rate / (double)L;
    e->pw = malloc((e->n ? e->n : 1) * sizeof(double));
    if (!e->pw) return -1;

    /* Mix first, then low-pass the COMPLEX samples before squaring.
       Squaring independent 5 ms correlations admits ~200 Hz of noise.
       An 80 ms Hann low-pass keeps the 5 ms time grid while rejecting
       program tones and retaining the shortest (170 ms) code pulse. */
    double *iq = calloc(2 * (e->n ? e->n : 1), sizeof(double));
    e->prefix = calloc(e->n + 1, sizeof(double));
    e->ci = calloc(e->n + 1, sizeof(double));
    e->cq = calloc(e->n + 1, sizeof(double));
    if (!iq || !e->prefix || !e->ci || !e->cq) {
        free(iq); free(e->prefix); free(e->ci); free(e->cq); free(e->pw);
        e->prefix = NULL; e->pw = NULL;
        return -1;
    }
    double w = 2.0 * M_PI * freq / (double)rate;
    double cd = cos(w), sd = sin(w), c = 1.0, sn = 0.0;
    for (size_t b = 0; b < e->n; ++b) {
        for (size_t i = 0; i < L; ++i) {
            double x = (double)s[b * L + i] / 32768.0;
            iq[2*b] += x * c / (double)L;
            iq[2*b+1] += x * sn / (double)L;
            double nc = c * cd - sn * sd;
            sn = sn * cd + c * sd; c = nc;
        }
        double r = hypot(c, sn);
        if (r > 0.0) { c /= r; sn /= r; }
        e->ci[b+1] = e->ci[b] + iq[2*b];
        e->cq[b+1] = e->cq[b] + iq[2*b+1];
    }
    int half = (int)lround(0.040 * e->sps);
    if (half < 1) half = 1;
    for (size_t b = 0; b < e->n; ++b) {
        double re = 0.0, im = 0.0, norm = 0.0;
        for (int j = -half; j <= half; ++j) {
            long k = (long)b + j;
            if (k < 0 || k >= (long)e->n) continue;
            double h = 0.5 + 0.5 * cos(M_PI * (double)j / half);
            re += h * iq[2*k]; im += h * iq[2*k+1]; norm += h;
        }
        e->pw[b] = 4.0 * (re*re + im*im) / (norm*norm);
        e->prefix[b+1] = e->prefix[b] + e->pw[b];
    }
    free(iq);
    return 0;
}

static void cenv_free(cenv_t *e) {
    free(e->pw); free(e->prefix); free(e->ci); free(e->cq); memset(e, 0, sizeof(*e));
}

static double cenv_mean(const cenv_t *e, double from, double to) {
    long i0 = lround(from), i1 = lround(to);
    if (i0 < 0) i0 = 0;
    if (i1 > (long)e->n) i1 = (long)e->n;
    if (i1 <= i0) return NAN;
    return (e->prefix[i1] - e->prefix[i0]) / (double)(i1 - i0);
}

/* Integrate complex voltage before taking power. This gives each symbol
   window its full processing gain instead of averaging noisy power bins. */
static double cenv_coherent(const cenv_t *e, double from, double to) {
    long i0 = lround(from), i1 = lround(to);
    if (i0 < 0) i0 = 0;
    if (i1 > (long)e->n) i1 = (long)e->n;
    if (i1 <= i0) return NAN;
    double re = (e->ci[i1]-e->ci[i0])/(i1-i0);
    double im = (e->cq[i1]-e->cq[i0])/(i1-i0);
    return 4.0*(re*re+im*im);
}

/* Per-second measurements, all in units of the envelope power. */
typedef struct {
    double a;   /* inside every pulse            0.045 - 0.185 s */
    double b;   /* inside "1" and marker         0.235 - 0.475 s */
    double c;   /* inside marker only            0.555 - 0.775 s */
    double d;   /* always quiet - noise floor    0.830 - 0.990 s */
} secstat_t;

static int sec_stats_env(const cenv_t *e, double sec_start, secstat_t *o) {
    double sps = e->sps;
    o->a = cenv_mean(e, sec_start + 0.045 * sps, sec_start + 0.185 * sps);
    o->b = cenv_mean(e, sec_start + 0.235 * sps, sec_start + 0.475 * sps);
    o->c = cenv_mean(e, sec_start + 0.555 * sps, sec_start + 0.775 * sps);
    o->d = cenv_mean(e, sec_start + 0.830 * sps, sec_start + 0.990 * sps);
    return isfinite(o->a) && isfinite(o->b) && isfinite(o->c) && isfinite(o->d);
}

static int sec_stats(const cenv_t *e, double sec_start, secstat_t *o) {
    double sps = e->sps;
    o->a = cenv_coherent(e, sec_start + 0.045 * sps, sec_start + 0.185 * sps);
    o->b = cenv_coherent(e, sec_start + 0.235 * sps, sec_start + 0.475 * sps);
    o->c = cenv_coherent(e, sec_start + 0.555 * sps, sec_start + 0.775 * sps);
    o->d = cenv_coherent(e, sec_start + 0.830 * sps, sec_start + 0.990 * sps);
    return isfinite(o->a) && isfinite(o->b) && isfinite(o->c) && isfinite(o->d);
}

/* Decide one symbol.  min_snr is the required pulse-to-floor power ratio
   in dB; below it the second is reported as unknown rather than guessed. */
static symbol_t sec_decide(const secstat_t *st, double min_snr_db, double *snr_db) {
    double floor_ = st->d > 0.0 ? st->d : 1e-30;
    double snr = st->a / floor_;
    if (snr_db) *snr_db = 10.0 * log10(snr > 0.0 ? snr : 1e-30);
    if (10.0 * log10(snr) < min_snr_db) return SYM_UNKNOWN;

    double ref = st->a - st->d;
    if (ref <= 0.0) return SYM_UNKNOWN;
    if ((st->c - st->d) > 0.45 * ref) return SYM_MARK;
    if ((st->b - st->d) > 0.45 * ref) return SYM_ONE;
    return SYM_ZERO;
}

/* ------------------------------------------------------------------ */
/* Time code interpretation                                           */
/* ------------------------------------------------------------------ */

static char symchar(symbol_t s) {
    switch (s) {
        case SYM_ZERO: return '0';
        case SYM_ONE:  return '1';
        case SYM_MARK: return 'P';
        case SYM_HOLE: return 'R';
        default:       return '?';
    }
}

static int bit_value(const symbol_t *sym, int sec, int *ok) {
    if (sec < 0 || sec >= MAX_SECONDS || sym[sec] == SYM_UNKNOWN) { *ok = 0; return 0; }
    if (sym[sec] == SYM_ONE) return 1;
    if (sym[sec] == SYM_ZERO) return 0;
    *ok = 0;
    return 0;
}

static int decode_weighted(const symbol_t *sym, const int *secs, const int *w,
                           size_t n, int *ok) {
    int v = 0;
    for (size_t i = 0; i < n; ++i) {
        int b = bit_value(sym, secs[i], ok);
        if (!*ok) return 0;
        if (b) v += w[i];
    }
    return v;
}

static int markers_ok(const symbol_t *sym) {
    static const int mk[] = {9, 19, 29, 39, 49, 59};
    for (size_t i = 0; i < sizeof(mk) / sizeof(mk[0]); ++i)
        if (sym[mk[i]] != SYM_MARK) return 0;
    return 1;
}

static int markers_present(const symbol_t *sym) {
    static const int mk[] = {9, 19, 29, 39, 49, 59};
    int n = 0;
    for (size_t i = 0; i < sizeof(mk) / sizeof(mk[0]); ++i)
        if (sym[mk[i]] == SYM_MARK) n++;
    return n;
}

/* Validate each decimal digit, not just the weighted total.  Invalid BCD
   such as a units digit of 12 must never be reported as a UTC time. */
static int frame_valid(const symbol_t *sym) {
    const int digit[][4] = {{4,5,6,7}, {51,52,53,54}, {10,11,12,13},
        {15,16,17,-1}, {20,21,22,23}, {25,26,-1,-1},
        {30,31,32,33}, {35,36,37,38}, {40,41,-1,-1}};
    const int maximum[] = {9,9,9,5,9,2,9,9,3};
    const int reserved[] = {1,8,14,18,24,27,28,34,42,43,44,45,46,47,48};
    int value[9], ok = 1;
    if (!markers_ok(sym) || sym[0] != SYM_HOLE) return 0;
    for (int i = 1; i < 60; ++i)
        if (i % 10 != 9 && sym[i] != SYM_ZERO && sym[i] != SYM_ONE) return 0;
    for (size_t j = 0; j < sizeof(reserved)/sizeof(*reserved); ++j)
        if (sym[reserved[j]] != SYM_ZERO) return 0;
    for (int j = 0; j < 9; ++j) {
        value[j] = 0;
        for (int k = 0; k < 4 && digit[j][k] >= 0; ++k)
            value[j] += bit_value(sym, digit[j][k], &ok) << k;
        if (value[j] > maximum[j]) return 0;
    }
    int yy = value[0] + 10*value[1];
    int hour = value[4] + 10*value[5];
    int day = value[6] + 10*value[7] + 100*value[8];
    return ok && hour <= 23 && day >= 1 && day <= (yy % 4 == 0 ? 366 : 365);
}

static int64_t frame_minutes(const symbol_t *s) {
    static const int y1[]={4,5,6,7}, yt[]={51,52,53,54}, w1[]={1,2,4,8}, wt[]={10,20,40,80};
    static const int ms[]={10,11,12,13,15,16,17}, mw[]={1,2,4,8,10,20,40};
    static const int hs[]={20,21,22,23,25,26}, hw[]={1,2,4,8,10,20};
    static const int ds[]={30,31,32,33,35,36,37,38,40,41}, dw[]={1,2,4,8,10,20,40,80,100,200};
    int ok=1;
    int yy=decode_weighted(s,y1,w1,4,&ok)+decode_weighted(s,yt,wt,4,&ok);
    int day=decode_weighted(s,ds,dw,10,&ok);
    int hh=decode_weighted(s,hs,hw,6,&ok), mm=decode_weighted(s,ms,mw,7,&ok);
    return ((int64_t)(365*yy+(yy+3)/4+day-1)*24+hh)*60+mm;
}

static void print_frame_status(const symbol_t *sym, const int *ticks_seen,
                        const double *tick_err_ms, int confirmed) {
    printf("FRAME ");
    for (int s = 0; s < 60; ++s) putchar(symchar(sym[s]));
    printf("  markers=%d/6\n", markers_present(sym));

    int ok = 1;
    const int y1s[] = {4,5,6,7};       const int y1w[] = {1,2,4,8};
    const int y10s[] = {51,52,53,54};  const int y10w[] = {10,20,40,80};
    const int mins[] = {10,11,12,13,15,16,17};
    const int minw[] = {1,2,4,8,10,20,40};
    const int hrs[] = {20,21,22,23,25,26};
    const int hrw[] = {1,2,4,8,10,20};
    const int days[] = {30,31,32,33,35,36,37,38,40,41};
    const int dayw[] = {1,2,4,8,10,20,40,80,100,200};
    const int dutsecs[] = {56,57,58};
    const int dutw[] = {1,2,4};

    int year = decode_weighted(sym, y1s, y1w, 4, &ok) +
               decode_weighted(sym, y10s, y10w, 4, &ok);
    int minute = decode_weighted(sym, mins, minw, 7, &ok);
    int hour = decode_weighted(sym, hrs, hrw, 6, &ok);
    int doy = decode_weighted(sym, days, dayw, 10, &ok);
    int dutmag = decode_weighted(sym, dutsecs, dutw, 3, &ok);
    int leap = bit_value(sym, 3, &ok);
    int dst2 = bit_value(sym, 2, &ok);
    int dutplus = bit_value(sym, 50, &ok);
    int dst1 = bit_value(sym, 55, &ok);

    if (!ok || !frame_valid(sym) || minute > 59 || hour > 23 ||
        doy < 1 || doy > 366 || year > 99) {
        printf("DECODE invalid/incomplete frame\n");
        return;
    }

    double dut1 = (dutmag == 0) ? 0.0 : (dutplus ? 1.0 : -1.0) * 0.1 * (double)dutmag;
    printf("%s YY=%02d DOY=%03d %02d:%02d:00%s\n", confirmed?"UTC  ":"UTC? ",
           year, doy, hour, minute, confirmed?"":" (unconfirmed timing hypothesis)");
    printf("DUT1  %+0.1f s\n", dut1);
    printf("LEAP  %s\n", leap ? "warning set" : "no warning");
    printf("DST   bits=%d%d\n", dst1, dst2);

    if (ticks_seen) {
        int n = 0;
        double sum = 0.0;
        for (int s = 1; s <= 58; ++s) {
            if (s == 29) continue;
            if (ticks_seen[s]) { n++; sum += fabs(tick_err_ms[s]); }
        }
        if (n) printf("TICKS %d observed, mean |phase error| %.2f ms\n", n, sum / n);
    }
}

static void print_frame(const symbol_t *sym, const int *ticks_seen,
                        const double *tick_err_ms) {
    print_frame_status(sym,ticks_seen,tick_err_ms,1);
}

/* ------------------------------------------------------------------ */
/* Offline frame search and decode                                    */
/* ------------------------------------------------------------------ */

/* Score a minute hypothesis.  Markers must show late energy, second 0 must
   be a hole, and every other second must show a leading pulse.  Everything
   is normalised by that second's own noise floor so fades do not dominate. */
static double frame_score(const cenv_t *e, double start_env) {
    static const int mk[] = {9, 19, 29, 39, 49, 59};
    double sps = e->sps;
    double score = 0.0;
    int nsec = 0;

    for (int k = 0; k < 60; ++k) {
        secstat_t st;
        if (!sec_stats_env(e, start_env + (double)k * sps, &st)) return -1e30;
        double fl = st.d > 0.0 ? st.d : 1e-30;
        int is_mk = 0;
        for (size_t i = 0; i < sizeof(mk) / sizeof(mk[0]); ++i)
            if (mk[i] == k) is_mk = 1;

        if (k == 0) {
            score += 2.0 * (fl - st.a) / (fl + st.a);        /* expect no pulse */
        } else if (is_mk) {
            score += (st.c - fl) / (st.c + fl);              /* expect 770 ms pulse */
        } else {
            score += 0.35 * (st.a - fl) / (st.a + fl);       /* expect a leading pulse */
        }
        nsec++;
    }
    return score / (double)nsec;
}

static int frame_hole(const cenv_t *e, double start) {
    double level[59]; int n = 0;
    for (int k = 1; k < 60; ++k) {
        secstat_t st;
        if (sec_stats(e,start+k*e->sps,&st)) level[n++] = st.a;
    }
    for (int i = 1; i < n; ++i) {
        double v = level[i]; int j = i;
        while (j && level[j-1] > v) { level[j] = level[j-1]; --j; }
        level[j] = v;
    }
    secstat_t hole;
    return n >= 30 && sec_stats(e,start,&hole) && hole.a < .40*level[n/2];
}

static int collect_frame(const cenv_t *e, double start, double min_snr,
                         symbol_t *sym) {
    for (int k = 0; k < 60; ++k) {
        secstat_t st;
        if (!sec_stats(e, start + k*e->sps, &st)) sym[k] = SYM_UNKNOWN;
        else if (k == 0)
            sym[k] = frame_hole(e,start) ? SYM_HOLE : SYM_UNKNOWN;
        else sym[k] = sec_decide(&st, min_snr, NULL);
    }
    return frame_valid(sym);
}

static int decode_at(const cenv_t *e, double t_sec, double min_snr,
                      int verbose, int *out_markers) {
    symbol_t sym[MAX_SECONDS];
    double snr[MAX_SECONDS];
    double start = t_sec * e->sps;

    for (int k = 0; k < 60; ++k) {
        secstat_t st;
        if (!sec_stats(e, start + (double)k * e->sps, &st)) {
            sym[k] = SYM_UNKNOWN;
            snr[k] = -99.0;
            continue;
        }
        sym[k] = sec_decide(&st, min_snr, &snr[k]);
        if (verbose)
            fprintf(stderr,
                    "sec=%02d sym=%c a=%.3e b=%.3e c=%.3e floor=%.3e snr=%+.1f dB\n",
                    k, symchar(sym[k]), st.a, st.b, st.c, st.d, snr[k]);
    }
    if (frame_hole(e,start)) sym[0] = SYM_HOLE;
    printf("SYNC  minute boundary at %.3f s into the recording\n", t_sec);
    print_frame(sym, NULL, NULL);
    if (out_markers) *out_markers = markers_present(sym);
    return frame_valid(sym);
}

static int offline_search(const cenv_t *e, int ncand, double min_snr,
                           int verbose) {
    if (e->n < (size_t)(60.0 * e->sps)) {
        fprintf(stderr, "search: recording is shorter than one minute\n");
        return 0;
    }
    size_t last = e->n - (size_t)(60.0 * e->sps);
    double step = 2.0;                     /* envelope samples = 10 ms */
    size_t ncan = (size_t)((double)last / step) + 1;

    double *sc = malloc(ncan * sizeof(double));
    if (!sc) return 0;
    double sum = 0.0, sum2 = 0.0;
    for (size_t i = 0; i < ncan; ++i) {
        sc[i] = frame_score(e, (double)i * step);
        sum += sc[i];
        sum2 += sc[i] * sc[i];
    }
    double mean = sum / (double)ncan;
    double sd = sqrt(fmax(0.0, sum2 / (double)ncan - mean * mean));

    /* With this many independent-ish trials the largest score from pure
       noise is already around mean + sd*sqrt(2 ln N).  Print that so a
       "best" candidate is not mistaken for a detection. */
    double chance = sqrt(2.0 * log((double)ncan));
    printf("SEARCH %zu minute hypotheses, score mean %.4f sd %.4f\n", ncan, mean, sd);
    printf("SEARCH chance-level best for this many trials is about %.1f sigma;\n"
           "       this heuristic is diagnostic; acceptance requires the full frame checks\n",
           chance);
    printf("SEARCH note: the marker pattern repeats every 10 s, so hypotheses\n"
           "       10 s apart score almost identically - only the second-0 hole\n"
           "       and a self-consistent BCD decode break the tie\n");

    /* Report the best few, keeping candidates apart so we do not print the
       same peak repeatedly.  Note that the marker pattern repeats every
       10 s, so genuine sync is only distinguished by the second-0 hole and
       by a self-consistent BCD decode. */
    double *keep = malloc((size_t)ncand * sizeof(double));
    if (!keep) { free(sc); return 0; }
    int nkeep = 0;
    unsigned char *valid = calloc(ncan, 1);
    if (!valid) { free(keep); free(sc); return 0; }
    int nvalid = 0;
    for (size_t i = 0; i < ncan; ++i) {
        symbol_t sym[60];
        valid[i] = (unsigned char)collect_frame(e, (double)i*step, min_snr, sym);
        nvalid += valid[i];
    }
    printf("SEARCH %d hypotheses pass hole, markers, reserved bits and BCD checks\n", nvalid);
    if (!nvalid) printf("SEARCH no complete validated frame; candidates below are diagnostic only\n");
    for (int r = 0; r < ncand; ++r) {
        double best = -1e30;
        size_t bi = 0;
        int found = 0;
        for (size_t i = 0; i < ncan; ++i) {
            double t = (double)i * step / e->sps;
            int near = 0;
            for (int j = 0; j < nkeep; ++j)
                if (fabs(t - keep[j]) < 5.0) near = 1;
            if (near || (nvalid && !valid[i])) continue;
            if (!found || sc[i] > best) { best = sc[i]; bi = i; found = 1; }
        }
        if (!found) break;
        double t = (double)bi * step / e->sps;
        keep[nkeep++] = t;
        printf("\nCAND  #%d  t=%.3f s  score=%.4f  (%.1f sigma above mean)\n",
               r + 1, t, best, sd > 0.0 ? (best - mean) / sd : 0.0);
        decode_at(e, t, min_snr, verbose && r == 0, NULL);
    }
    free(valid);
    free(keep);
    free(sc);
    return nvalid ? nkeep : 0;
}

static volatile sig_atomic_t stop_flag = 0;

/* Follow code-pulse timing through gradual capture gaps.  The known
   marker/reserved-bit layout guides timing only: emitted symbols are
   measured again independently, so the tracker cannot fill in missing
   bits merely because it expects a marker or a zero. */
static double tracker_quality(const double z[5], int type) {
    double hi = 0.0, lo = 0.0, var = 0.0;
    int nh = type + 1;
    for (int j = 0; j < 5; ++j) {
        if (j < nh) hi += z[j]; else lo += z[j];
    }
    hi /= nh; lo /= 5 - nh;
    for (int j = 0; j < 5; ++j) {
        double diff = z[j] - (j < nh ? hi : lo);
        var += diff*diff;
    }
    double total = hi + lo + 1e-30;
    return (hi-lo)/total - 0.3*var/(total*total);
}

static int offline_track(const cenv_t *e, double min_snr, int verbose) {
    size_t n = e->n;
    int low = (int)lround(.870*e->sps), high = (int)lround(1.030*e->sps);
    int tail = (int)ceil(.870*e->sps);
    size_t ring = (size_t)high + 1;
    if (n < (size_t)(60*e->sps)) return 0;
    if (n > SIZE_MAX/60) return 0;
    float *dp = malloc(ring*60*sizeof(float));
    unsigned char *back = calloc(n*60, 1);
    size_t *times = malloc(n*sizeof(size_t));
    unsigned char *phases = malloc(n);
    if (!dp || !back || !times || !phases) {
        fprintf(stderr, "track: out of memory\n");
        free(dp); free(back); free(times); free(phases); return 0;
    }
    for (size_t j = 0; j < ring*60; ++j) dp[j] = -1e20f;
    static const int reserved[] = {1,8,14,18,24,27,28,34,42,43,44,45,46,47,48};
    int fixed_zero[60] = {0};
    for (size_t j = 0; j < sizeof(reserved)/sizeof(*reserved); ++j)
        fixed_zero[reserved[j]] = 1;
    double best_end = -1e20;
    size_t end_t = 0;
    int end_k = 0;
    for (size_t t = 0; t + tail <= n && !stop_flag; ++t) {
        secstat_t st;
        if (!sec_stats_env(e, (double)t, &st)) continue;
        double prev = t >= (size_t)lround(.120*e->sps)
            ? cenv_mean(e, (double)t-.120*e->sps, (double)t-.020*e->sps) : st.d;
        double z[] = {st.a,st.b,st.c,st.d,prev}, q[4];
        for (int j = 0; j < 3; ++j) q[j] = tracker_quality(z,j);
        q[3] = .5 - (st.a+st.b+st.c-3*st.d)/(st.a+st.b+st.c+3*st.d+1e-30);
        double pre_edge = cenv_mean(e,fmax(0.0,(double)t-.010*e->sps),(double)t+.020*e->sps);
        double post_edge = cenv_mean(e,(double)t+.040*e->sps,(double)t+.070*e->sps);
        double edge = (post_edge-pre_edge)/(post_edge+pre_edge+1e-30);
        for (int k = 0; k < 60; ++k) {
            int symbol = k == 0 ? 3 : (k%10 == 9 ? 2 :
                         (fixed_zero[k] ? 0 : (q[0]>q[1] ? 0 : 1)));
            double base = q[symbol] - .4 + (symbol == 3 ? 0.0 : .65*edge);
            double best = t < (size_t)ceil(1.15*e->sps) ? base : -1e20;
            int bd = 0;
            for (int dur = low; dur <= high && (size_t)dur <= t; ++dur) {
                double dev = ((double)dur/e->sps - .985)*200.0;
                double v = dp[((t-dur)%ring)*60+(k+59)%60] - .0009*dev*dev + base;
                if (v > best) { best = v; bd = dur; }
            }
            dp[(t%ring)*60+k] = (float)best;
            back[t*60+k] = (unsigned char)bd;
            if (t + tail + (size_t)high >= n && best > best_end) {
                best_end = best; end_t = t; end_k = k;
            }
        }
    }
    size_t count = 0, t = end_t;
    int k = end_k;
    for (;;) {
        times[count] = t; phases[count++] = (unsigned char)k;
        unsigned dur = back[t*60+k];
        if (!dur || t < dur) break;
        t -= dur; k = (k+59)%60;
    }
    symbol_t frame[60];
    for (int j = 0; j < 60; ++j) frame[j] = SYM_UNKNOWN;
    int have_start = 0, valid = 0, total = 0;
    size_t capacity = count/60+2;
    symbol_t (*frames)[60] = calloc(capacity,sizeof(*frames));
    double *starts = calloc(capacity,sizeof(double));
    if (!frames || !starts) {
        free(frames); free(starts); free(dp); free(back); free(times); free(phases); return 0;
    }
    double frame_start = 0.0, last = -1.0, sum_step = 0.0;
    double frame_high[60] = {0};
    int steps = 0;
    printf("TRACK adaptive second boundaries (timing hypothesis; bits are independently measured)\n");
    for (size_t j = count; j-- > 0;) {
        t = times[j]; k = phases[j];
        if (last >= 0.0) { sum_step += (double)t/e->sps-last; steps++; }
        last = (double)t/e->sps;
        if (k == 0) { have_start = 1; frame_start = last; }
        secstat_t st;
        if (!sec_stats(e,(double)t,&st)) continue;
        double end = j ? (double)times[j-1] : (double)n;
        double quiet_end = fmin((double)t+.990*e->sps,end-.010*e->sps);
        st.d = cenv_coherent(e,(double)t+.830*e->sps,quiet_end);
        double snr;
        symbol_t measured = sec_decide(&st,min_snr,&snr);
        if (k == 0) measured = st.a < pow(10.0,min_snr/10.0)*fmax(st.d,1e-30)
                                      ? SYM_HOLE : SYM_UNKNOWN;
        frame[k] = measured;
        frame_high[k] = st.a;
        if (verbose && have_start)
            fprintf(stderr,"track sec=%02d t=%.3f sym=%c snr=%+.1f dB\n",
                    k,last,symchar(measured),snr);
        if (k == 59) {
            if (have_start) {
                double level = 0.0;
                for (int v = 1; v < 60; ++v) level += frame_high[v]/59.0;
                frame[0] = frame_high[0] < .40*level ? SYM_HOLE : SYM_UNKNOWN;
                memcpy(frames[total],frame,sizeof(frame));
                starts[total++] = frame_start;
            }
            for (int v = 0; v < 60; ++v) frame[v] = SYM_UNKNOWN;
            have_start = 0;
        }
    }
    for (int i = 0; i < total; ++i) {
        int confirmed = 0;
        if (frame_valid(frames[i])) {
            int64_t mi = frame_minutes(frames[i]);
            for (int j = 0; j < total; ++j) {
                if (i == j || abs(i-j)>2 || !frame_valid(frames[j])) continue;
                int64_t delta = frame_minutes(frames[j])-mi;
                const int64_t century = 36525LL*24*60;
                if ((delta-(j-i)+century)%century == 0) confirmed = 1;
            }
        }
        printf("SYNC  tracked minute boundary at %.3f s into the recording\n",starts[i]);
        print_frame_status(frames[i],NULL,NULL,confirmed);
        valid += confirmed;
    }
    free(frames); free(starts);
    printf("TRACK %d/%d complete frames confirmed by a second consistent minute; mean recorded second %.6f s\n",
           valid,total,steps ? sum_step/steps : 0.0);
    free(dp); free(back); free(times); free(phases);
    return valid;
}

/* ------------------------------------------------------------------ */
/* Streaming decoder (ALSA / stdin)                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    double c, s;
    double cd, sd;
    double i, q;
    double alpha;
} iqdet_t;

typedef struct {
    uint64_t n;
    double sum;
} accum_t;

typedef struct {
    double *buf;
    double *c1000, *s1000;
    double *c1500, *s1500;
    size_t nwin;
    size_t pos;
    size_t filled;
    unsigned decim;
    unsigned decim_count;
    double env1000;
    double env1500;
    double rms;
} timing_corr_t;

typedef struct {
    unsigned rate;
    uint64_t sample_index;

    iqdet_t d100;
    iqdet_t m500, m600;                 /* harmonic sources */
    iqdet_t m990, m1000, m1010;
    iqdet_t m1490, m1500, m1510;
    timing_corr_t timing;

    int tone1000_present, tone1500_present;
    uint64_t tone1000_start, tone1500_start;
    uint64_t tone1000_last_present, tone1500_last_present;
    int minute_marker_armed_1000, minute_marker_armed_1500;

    int synced;
    double frame_start;
    symbol_t symbols[MAX_SECONDS];
    int ticks_seen[MAX_SECONDS];
    double tick_error_ms[MAX_SECONDS];

    accum_t a_hi, a_mid1, a_mid2, a_lo;
    int accum_sec;

    double tick_ratio_threshold;
    double min_rms;
    double phase_gain;
    double tone_offset;
    double harm_reject;                  /* 1000 Hz must exceed this * 500 Hz */
    double min_snr_db;
    int verbose;
    unsigned decoded_frames;
    uint64_t harm_blocked;
} decoder_t;


static void on_signal(int sig) { (void)sig; stop_flag = 1; }

static void iq_init(iqdet_t *d, double freq, double cutoff, double rate) {
    double w = 2.0 * M_PI * freq / rate;
    d->c = 1.0; d->s = 0.0;
    d->cd = cos(w); d->sd = sin(w);
    d->i = 0.0; d->q = 0.0;
    d->alpha = 1.0 - exp(-2.0 * M_PI * cutoff / rate);
}

static double iq_step(iqdet_t *d, double x) {
    double mix_i = x * d->c;
    double mix_q = x * d->s;
    d->i += d->alpha * (mix_i - d->i);
    d->q += d->alpha * (mix_q - d->q);
    double nc = d->c * d->cd - d->s * d->sd;
    double ns = d->s * d->cd + d->c * d->sd;
    d->c = nc; d->s = ns;
    double r2 = d->c * d->c + d->s * d->s;
    if (r2 < 0.999999 || r2 > 1.000001) {
        double r = sqrt(r2);
        d->c /= r; d->s /= r;
    }
    return 2.0 * hypot(d->i, d->q);
}

static int timing_corr_init(timing_corr_t *t, unsigned rate, double tone_offset) {
    memset(t, 0, sizeof(*t));
    t->nwin = (size_t)llround(0.010 * (double)rate);
    if (t->nwin < 16) t->nwin = 16;
    t->decim = rate / 1000u;
    if (t->decim < 1) t->decim = 1;
    t->buf = calloc(t->nwin, sizeof(double));
    t->c1000 = malloc(t->nwin * sizeof(double));
    t->s1000 = malloc(t->nwin * sizeof(double));
    t->c1500 = malloc(t->nwin * sizeof(double));
    t->s1500 = malloc(t->nwin * sizeof(double));
    if (!t->buf || !t->c1000 || !t->s1000 || !t->c1500 || !t->s1500) return -1;
    for (size_t k = 0; k < t->nwin; ++k) {
        double ph1 = 2.0 * M_PI * (1000.0 + tone_offset) * (double)k / (double)rate;
        double ph15 = 2.0 * M_PI * (1500.0 + tone_offset) * (double)k / (double)rate;
        t->c1000[k] = cos(ph1); t->s1000[k] = sin(ph1);
        t->c1500[k] = cos(ph15); t->s1500[k] = sin(ph15);
    }
    return 0;
}

static void timing_corr_free(timing_corr_t *t) {
    free(t->buf); free(t->c1000); free(t->s1000); free(t->c1500); free(t->s1500);
    memset(t, 0, sizeof(*t));
}

static int timing_corr_step(timing_corr_t *t, double x) {
    t->buf[t->pos] = x;
    t->pos = (t->pos + 1) % t->nwin;
    if (t->filled < t->nwin) t->filled++;
    if (++t->decim_count < t->decim) return 0;
    t->decim_count = 0;
    if (t->filled < t->nwin) return 0;

    double i1=0.0,q1=0.0,i15=0.0,q15=0.0,ss=0.0;
    for (size_t k = 0; k < t->nwin; ++k) {
        size_t idx = (t->pos + k) % t->nwin;
        double v = t->buf[idx];
        i1 += v * t->c1000[k]; q1 += v * t->s1000[k];
        i15 += v * t->c1500[k]; q15 += v * t->s1500[k];
        ss += v * v;
    }
    double norm = 2.0 / (double)t->nwin;
    t->env1000 = norm * hypot(i1, q1);
    t->env1500 = norm * hypot(i15, q15);
    t->rms = sqrt(ss / (double)t->nwin);
    return 1;
}

static void accum_reset(accum_t *a) { a->n = 0; a->sum = 0.0; }
static void accum_add(accum_t *a, double x) { a->n++; a->sum += x; }
static double accum_mean(const accum_t *a) { return a->n ? a->sum / (double)a->n : NAN; }

static void finalize_symbol(decoder_t *d, int sec) {
    if (sec <= 0 || sec >= 60) return;
    double hi = accum_mean(&d->a_hi);
    double m1 = accum_mean(&d->a_mid1);
    double m2 = accum_mean(&d->a_mid2);
    double lo = accum_mean(&d->a_lo);

    secstat_t st = {hi,m1,m2,lo};
    double snr_db = -99.0;
    symbol_t out = isfinite(hi) && isfinite(m1) && isfinite(m2) && isfinite(lo)
        ? sec_decide(&st,d->min_snr_db,&snr_db) : SYM_UNKNOWN;
    d->symbols[sec] = out;
    if (d->verbose)
        fprintf(stderr, "sec=%02d sym=%c hi=%.3e m1=%.3e m2=%.3e lo=%.3e snr=%+.1f dB\n",
                sec, symchar(out), hi, m1, m2, lo, snr_db);
}

static void finish_frame_if_ready(decoder_t *d) {
    if (!d->synced) return;
    if (d->accum_sec >= 1 && d->accum_sec <= 59) finalize_symbol(d, d->accum_sec);
    if (d->accum_sec >= 58) {
        print_frame(d->symbols, d->ticks_seen, d->tick_error_ms);
        d->decoded_frames += (unsigned)frame_valid(d->symbols);
    }
    else if (d->verbose)
        fprintf(stderr, "dropping incomplete frame (last second %d)\n", d->accum_sec);
    d->synced = 0;
    d->accum_sec = -1;
}

static void reset_frame(decoder_t *d, double frame_start) {
    if (d->synced) finish_frame_if_ready(d);
    d->synced = 1;
    d->frame_start = frame_start;
    d->accum_sec = -1;
    for (int i = 0; i < MAX_SECONDS; ++i) {
        d->symbols[i] = SYM_UNKNOWN;
        d->ticks_seen[i] = 0;
        d->tick_error_ms[i] = 0.0;
    }
    d->symbols[0] = SYM_HOLE;
    accum_reset(&d->a_hi); accum_reset(&d->a_mid1);
    accum_reset(&d->a_mid2); accum_reset(&d->a_lo);
    if (d->verbose)
        fprintf(stderr, "sync: minute boundary at %.3f s\n",
                frame_start / (double)d->rate);
}

static void begin_second(decoder_t *d, int sec) {
    d->accum_sec = sec;
    accum_reset(&d->a_hi); accum_reset(&d->a_mid1);
    accum_reset(&d->a_mid2); accum_reset(&d->a_lo);
}

/* Power-domain accumulation over the same windows the offline path uses. */
static void update_symbol_windows(decoder_t *d, double env100) {
    if (!d->synced) return;
    double rel = ((double)d->sample_index - d->frame_start) / (double)d->rate;
    if (rel < 0.0) return;

    int sec = (int)floor(rel);
    double frac = rel - floor(rel);

    if (sec >= 60) {
        double next_frame = d->frame_start + 60.0 * (double)d->rate;
        finish_frame_if_ready(d);
        reset_frame(d, next_frame);
        return;
    }
    if (sec != d->accum_sec) {
        if (d->accum_sec >= 1 && d->accum_sec <= 59) finalize_symbol(d, d->accum_sec);
        begin_second(d, sec);
    }
    if (sec == 0) return;

    double p = env100 * env100;
    if (frac >= 0.045 && frac < 0.185) accum_add(&d->a_hi, p);
    if (frac >= 0.235 && frac < 0.475) accum_add(&d->a_mid1, p);
    if (frac >= 0.555 && frac < 0.775) accum_add(&d->a_mid2, p);
    if (frac >= 0.830 && frac < 0.990) accum_add(&d->a_lo, p);
}

static void register_tick(decoder_t *d, double tick_start) {
    if (!d->synced) return;
    double kf = (tick_start - d->frame_start) / (double)d->rate;
    int sec = (int)llround(kf);
    if (sec < 0 || sec >= 60) return;
    double expected = d->frame_start + (double)sec * d->rate;
    double err = tick_start - expected;
    double err_ms = 1000.0 * err / (double)d->rate;
    if (fabs(err_ms) > 20.0) return;
    if (sec == 29 || sec == 59) return;
    d->ticks_seen[sec] = 1;
    d->tick_error_ms[sec] = err_ms;
    if (sec > 0 && fabs(err_ms) < 12.0) d->frame_start += d->phase_gain * err;
}

static int tone_present(double env, double rms, double ratio, double min_rms) {
    if (rms < min_rms) return 0;
    return (env / (rms + 1e-12)) >= ratio;
}

static void update_timing(decoder_t *d, double env1000, double env1500, double rms,
                          double m500, double m600,
                          double m990, double m1000, double m1010,
                          double m1490, double m1500, double m1510) {
    uint64_t now = d->sample_index;
    uint64_t gap_tol = (uint64_t)(0.020 * d->rate);
    uint64_t marker_len = (uint64_t)(0.200 * d->rate);

    int p1000 = tone_present(env1000, rms, d->tick_ratio_threshold, d->min_rms);
    int p1500 = tone_present(env1500, rms, d->tick_ratio_threshold, d->min_rms);

    /* Harmonic gate.  1000 and 1500 Hz are the 2nd and 3rd harmonics of the
       500 Hz program tone; 1200 and 1800 Hz are 600 Hz harmonics.  When a
       program tone is running, distortion products sit exactly on the WWV
       timing frequencies, so require the timing tone to dominate its
       possible parent before believing it. */
    double parent = fmax(m500, m600);
    if (p1000 && parent > 1e-4 && m1000 < d->harm_reject * parent) {
        p1000 = 0;
        d->harm_blocked++;
    }
    if (p1500 && m500 > 1e-4 && m1500 < d->harm_reject * m500) p1500 = 0;

    if (p1000) {
        if (!d->tone1000_present) {
            if (d->tone1000_last_present == 0 || now - d->tone1000_last_present > gap_tol) {
                d->tone1000_start = now;
                d->minute_marker_armed_1000 = 1;
                register_tick(d, (double)now);
            }
            d->tone1000_present = 1;
        }
        d->tone1000_last_present = now;
    } else if (d->tone1000_present && now - d->tone1000_last_present > gap_tol) {
        d->tone1000_present = 0;
        d->minute_marker_armed_1000 = 0;
    }

    if (p1500) {
        if (!d->tone1500_present) {
            if (d->tone1500_last_present == 0 || now - d->tone1500_last_present > gap_tol) {
                d->tone1500_start = now;
                d->minute_marker_armed_1500 = 1;
            }
            d->tone1500_present = 1;
        }
        d->tone1500_last_present = now;
    } else if (d->tone1500_present && now - d->tone1500_last_present > gap_tol) {
        d->tone1500_present = 0;
        d->minute_marker_armed_1500 = 0;
    }

    if (d->minute_marker_armed_1000 && now - d->tone1000_start >= marker_len) {
        double neighbor = fmax(m990, m1010);
        int spectral_ok = (m1000 > 1.30 * neighbor) && (m1000 > 2e-4);
        if (spectral_ok) {
            if (!d->synced) reset_frame(d, (double)d->tone1000_start);
            d->minute_marker_armed_1000 = 0;
        } else if (now - d->tone1000_start > (uint64_t)(0.950 * d->rate)) {
            d->minute_marker_armed_1000 = 0;
        }
    }
    if (d->minute_marker_armed_1500 && now - d->tone1500_start >= marker_len) {
        double neighbor = fmax(m1490, m1510);
        int spectral_ok = (m1500 > 1.30 * neighbor) && (m1500 > 2e-4);
        if (spectral_ok) {
            if (!d->synced) reset_frame(d, (double)d->tone1500_start);
            d->minute_marker_armed_1500 = 0;
        } else if (now - d->tone1500_start > (uint64_t)(0.950 * d->rate)) {
            d->minute_marker_armed_1500 = 0;
        }
    }
}

static void decoder_init(decoder_t *d, unsigned rate, int verbose,
                         double tone_offset, double harm_reject, double min_snr) {
    memset(d, 0, sizeof(*d));
    d->rate = rate;
    d->verbose = verbose;
    d->accum_sec = -1;
    d->tick_ratio_threshold = 0.60;
    d->min_rms = 5e-5;
    d->phase_gain = 0.05;
    d->tone_offset = tone_offset;
    d->harm_reject = harm_reject;
    d->min_snr_db = min_snr;

    iq_init(&d->d100, 100.0 + tone_offset, 4.0, rate);
    iq_init(&d->m500,  500.0 + tone_offset, 8.0, rate);
    iq_init(&d->m600,  600.0 + tone_offset, 8.0, rate);
    iq_init(&d->m990,  990.0 + tone_offset, 3.0, rate);
    iq_init(&d->m1000, 1000.0 + tone_offset, 3.0, rate);
    iq_init(&d->m1010, 1010.0 + tone_offset, 3.0, rate);
    iq_init(&d->m1490, 1490.0 + tone_offset, 3.0, rate);
    iq_init(&d->m1500, 1500.0 + tone_offset, 3.0, rate);
    iq_init(&d->m1510, 1510.0 + tone_offset, 3.0, rate);
    if (timing_corr_init(&d->timing, rate, tone_offset) < 0) {
        fprintf(stderr, "out of memory initializing timing correlator\n");
        exit(1);
    }
}

static void decoder_process_sample(decoder_t *d, int16_t s) {
    double x = (double)s / 32768.0;
    double e100 = iq_step(&d->d100, x);
    double m500 = iq_step(&d->m500, x);
    double m600 = iq_step(&d->m600, x);
    double m990  = iq_step(&d->m990, x);
    double m1000 = iq_step(&d->m1000, x);
    double m1010 = iq_step(&d->m1010, x);
    double m1490 = iq_step(&d->m1490, x);
    double m1500 = iq_step(&d->m1500, x);
    double m1510 = iq_step(&d->m1510, x);

    if (timing_corr_step(&d->timing, x))
        update_timing(d, d->timing.env1000, d->timing.env1500, d->timing.rms,
                      m500, m600, m990, m1000, m1010, m1490, m1500, m1510);
    update_symbol_windows(d, e100);
    d->sample_index++;
}

static void decoder_process(decoder_t *d, const int16_t *buf, size_t n) {
    for (size_t i = 0; i < n; ++i) decoder_process_sample(d, buf[i]);
}

static void decoder_flush(decoder_t *d) { finish_frame_if_ready(d); }

/* ------------------------------------------------------------------ */
/* Tone offset estimation                                             */
/* ------------------------------------------------------------------ */

static double tone_energy(const int16_t *buf, size_t n, unsigned rate, double freq) {
    double w = 2.0 * M_PI * freq / (double)rate;
    double cd = cos(w), sd = sin(w);
    double c = 1.0, ss = 0.0;
    double re = 0.0, im = 0.0;
    double mean = 0.0;
    for (size_t i = 0; i < n; ++i) mean += (double)buf[i];
    mean /= (double)n;
    for (size_t i = 0; i < n; ++i) {
        double x = ((double)buf[i] - mean) / 32768.0;
        re += x * c;
        im += x * ss;
        double nc = c * cd - ss * sd;
        double ns = ss * cd + c * sd;
        c = nc; ss = ns;
        if ((i & 4095u) == 4095u) {
            double r = hypot(c, ss);
            if (r > 0.0) { c /= r; ss /= r; }
        }
    }
    return (re * re + im * im) / ((double)n * (double)n + 1e-30);
}

static double estimate_tone_offset(const int16_t *buf, size_t n, unsigned rate,
                                   int verbose) {
    /* Coherent integration over more than a few seconds is pointless on HF
       (ionospheric Doppler smears the line), so score several short blocks
       and add the scores. */
    size_t blk = (size_t)rate * 4u;
    if (blk > n) blk = n;
    size_t nblk = n / blk;
    if (nblk > 8) nblk = 8;
    if (nblk == 0) nblk = 1;

    double best_off = 0.0, best_score = -1.0;
    for (int k = -80; k <= 80; ++k) {
        double off = 0.5 * (double)k;
        double score = 0.0;
        for (size_t b = 0; b < nblk; ++b) {
            const int16_t *p = buf + b * blk;
            score += log1p(1e8 * tone_energy(p, blk, rate,  100.0 + off)) +
                     log1p(1e8 * tone_energy(p, blk, rate,  500.0 + off)) +
                     log1p(1e8 * tone_energy(p, blk, rate,  600.0 + off)) +
                     0.8 * log1p(1e8 * tone_energy(p, blk, rate, 1000.0 + off)) +
                     0.4 * log1p(1e8 * tone_energy(p, blk, rate, 1500.0 + off));
        }
        if (score > best_score) { best_score = score; best_off = off; }
    }
    double coarse = best_off;
    best_score = -1.0;
    for (int j = -5; j <= 5; ++j) {
        double off = coarse + 0.1 * (double)j;
        double score = 0.0;
        for (size_t b = 0; b < nblk; ++b) {
            const int16_t *p = buf + b * blk;
            score += log1p(1e8 * tone_energy(p, blk, rate,  100.0 + off)) +
                     log1p(1e8 * tone_energy(p, blk, rate,  500.0 + off)) +
                     log1p(1e8 * tone_energy(p, blk, rate,  600.0 + off)) +
                     0.8 * log1p(1e8 * tone_energy(p, blk, rate, 1000.0 + off)) +
                     0.4 * log1p(1e8 * tone_energy(p, blk, rate, 1500.0 + off));
        }
        if (score > best_score) { best_score = score; best_off = off; }
    }
    if (verbose)
        fprintf(stderr,
                "auto-offset: estimated audio shift %+.1f Hz "
                "(data %.1f, tick %.1f, hour %.1f Hz)\n",
                best_off, 100.0 + best_off, 1000.0 + best_off, 1500.0 + best_off);
    return best_off;
}

/* ------------------------------------------------------------------ */
/* Offline signal analysis                                            */
/* ------------------------------------------------------------------ */

static void analyze(const pcm_t *p, double off, const cenv_t *e) {
    unsigned rate = p->rate;
    printf("\n=== signal analysis ===\n");
    printf("duration           %.1f s (%zu samples at %u Hz)\n",
           (double)p->n / rate, p->n, rate);
    printf("audio tone offset  %+.2f Hz  (100 Hz code lands at %.2f Hz)\n",
           off, 100.0 + off);

    /* Effective resolution: how many distinct sample values are in use. */
    {
        int seen[4096];
        memset(seen, 0, sizeof(seen));
        int distinct = 0;
        for (size_t i = 0; i < p->n; ++i) {
            int b = (p->s[i] >> 4) & 0xFFF;
            if (!seen[b]) { seen[b] = 1; distinct++; }
        }
        printf("sample resolution  ~%d distinct coarse levels used\n", distinct);
    }

    /* Passband profile: is the receiver passing 100 Hz at all?  A receiver
       in SSB mode typically rolls off below 300 Hz, which puts the time
       code subcarrier under the noise even when the station is loud. */
    printf("\naudio passband (mean power in %.0f Hz bins):\n", 1.0 / PROBE_BLOCK);
    static const double probe[] = {50, 100, 150, 200, 300, 400, 700, 1000,
                                   1500, 2000, 2500, 3000, 3500};
    double ref = band_power(p->s, p->n, rate, 2000.0, PROBE_BLOCK);
    for (size_t i = 0; i < sizeof(probe) / sizeof(probe[0]); ++i) {
        double pw = band_power(p->s, p->n, rate, probe[i] + off, PROBE_BLOCK);
        printf("  %6.0f Hz  %10.3e  %+6.1f dB rel 2 kHz\n",
               probe[i], pw, 10.0 * log10(fmax(pw, 1e-30) / fmax(ref, 1e-30)));
    }

    /* 100 Hz code link budget.  Noise comes from +/-15 and +/-20 Hz, clear
       of the code's own modulation sidebands but in the same part of the
       receiver passband.
         N0      = P_noise * Tb / 2      (Tb = PROBE_BLOCK)
         E/N0    = (S_on / P_noise) * (T / Tb)
       with S_on the on-pulse line power, i.e. the time average divided by
       the roughly 45% duty cycle of the code. */
    double pc = band_power(p->s, p->n, rate, 100.0 + off, PROBE_BLOCK);
    double pn = 0.25 * (band_power(p->s, p->n, rate,  80.0 + off, PROBE_BLOCK) +
                        band_power(p->s, p->n, rate,  85.0 + off, PROBE_BLOCK) +
                        band_power(p->s, p->n, rate, 115.0 + off, PROBE_BLOCK) +
                        band_power(p->s, p->n, rate, 120.0 + off, PROBE_BLOCK));
    double sig = pc - pn;
    printf("\n100 Hz time code:\n");
    printf("  code line power   %.3e\n", pc);
    printf("  local noise       %.3e\n", pn);
    if (sig <= 0.0) {
        printf("  the 100 Hz subcarrier is not detectable above the local noise\n");
        printf("  VERDICT: code not detected at this tuning; absence is not established\n");
    } else {
        printf("  line SNR          %+.1f dB in a %.0f Hz bandwidth\n",
               10.0 * log10(sig / fmax(pn, 1e-30)), 1.0 / PROBE_BLOCK);
        double s_on = sig / 0.45;
        double tb = PROBE_BLOCK;
        double e0 = 10.0 * log10(s_on * 0.17 / (tb * fmax(pn, 1e-30)));
        double e1 = 10.0 * log10(s_on * 0.47 / (tb * fmax(pn, 1e-30)));
        double em = 10.0 * log10(s_on * 0.77 / (tb * fmax(pn, 1e-30)));
        printf("  estimated E/N0    %+.1f dB for a 170 ms '0', %+.1f dB for a"
               " 470 ms '1', %+.1f dB for a 770 ms marker\n", e0, e1, em);
        printf("  hard bit decisions want about +12 dB, so this recording is"
               " %.0f dB short\n", 12.0 - e0);
        if (e0 < 8.0)
            printf("  VERDICT: too weak for reliable per-minute decoding\n");
        else
            printf("  VERDICT: code energy detected; timing and full-frame validity still need checking\n");
    }

    /* Program tone segments.  WWV runs 45 s of tone starting 1 s after each
       minute, so segment starts should be 60 s apart and 45 s long.  Any
       systematic shortfall means the recording is missing audio. */
    printf("\nprogram tone segments (500/600 Hz, expect 45.0 s starting"
           " 1 s after each minute):\n");
    {
        size_t sec = p->n / rate;
        double prev_start = -1.0;
        int in_seg = 0, which = 0;
        double seg_start = 0.0;
        for (size_t k = 0; k < sec; ++k) {
            const int16_t *q = p->s + k * rate;
            double p5 = band_power(q, rate, rate, 500.0 + off, 0.1);
            double p6 = band_power(q, rate, rate, 600.0 + off, 0.1);
            double pnz = 0.25 * (band_power(q, rate, rate, 760.0 + off, 0.1) +
                                 band_power(q, rate, rate, 820.0 + off, 0.1) +
                                 band_power(q, rate, rate, 1300.0 + off, 0.1) +
                                 band_power(q, rate, rate, 1350.0 + off, 0.1));
            int w = 0;
            if (p5 > 10.0 * pnz || p6 > 10.0 * pnz) w = (p5 >= p6) ? 500 : 600;
            if (w && !in_seg) { in_seg = 1; which = w; seg_start = (double)k; }
            else if (in_seg && (!w || w != which)) {
                double dur = (double)k - seg_start;
                if (dur >= 5.0) {
                    printf("  %3d Hz  %8.1f -> %8.1f s  (%5.1f s)", which,
                           seg_start, (double)k, dur);
                    if (prev_start >= 0.0)
                        printf("  %+6.1f s since previous start", seg_start - prev_start);
                    putchar('\n');
                    prev_start = seg_start;
                }
                in_seg = w ? 1 : 0;
                which = w;
                seg_start = (double)k;
            }
        }
    }

    /* Timing continuity is diagnostic, not proof of dropped buffers.
       The code repeats every second, so the envelope
       has a 1 Hz Fourier component whose phase is the second boundary.
       Measured over consecutive windows that phase must stay put: a sound
       card off by even 100 ppm only slips 3 ms per 30 s.  Windows that
       disagree by much more than that mean the recording itself has lost
       audio, and no amount of DSP will put it back. */
    if (e && e->n > (size_t)(60.0 * e->sps)) {
        printf("\ntiming continuity (1 Hz line in the code envelope):\n");
        size_t w = (size_t)(30.0 * e->sps);
        double prev_ph = -1.0, prev_time = 0.0;
        int prev_ok = 0;
        double worst = 0.0, worst_elapsed = 30.0;
        int nslip = 0;
        for (size_t s = 0; s + w < e->n; s += w) {
            double mean = 0.0;
            for (size_t i = 0; i < w; ++i) mean += e->pw[s + i];
            mean /= (double)w;

            double re = 0.0, im = 0.0;
            for (size_t i = 0; i < w; ++i) {
                double a = 2.0 * M_PI * (double)i / e->sps;
                double v = e->pw[s + i] - mean;
                re += v * cos(a);
                im -= v * sin(a);
            }
            re /= (double)w; im /= (double)w;
            double mag = hypot(re, im);

            static const double nzf[] = {0.7, 0.83, 1.17, 1.3, 1.43};
            double nz = 0.0;
            for (size_t j = 0; j < sizeof(nzf) / sizeof(nzf[0]); ++j) {
                double r2 = 0.0, i2 = 0.0;
                for (size_t i = 0; i < w; ++i) {
                    double a = 2.0 * M_PI * nzf[j] * (double)i / e->sps;
                    double v = e->pw[s + i] - mean;
                    r2 += v * cos(a);
                    i2 -= v * sin(a);
                }
                r2 /= (double)w; i2 /= (double)w;
                nz += r2 * r2 + i2 * i2;
            }
            nz = sqrt(nz / (double)(sizeof(nzf) / sizeof(nzf[0])));

            double ratio = mag / fmax(nz, 1e-30);
            double ph = atan2(-im, re) / (2.0 * M_PI);
            while (ph < 0.0) ph += 1.0;
            int ok = ratio >= 2.5;
            printf("  %5.0f s  line/noise %5.2f  second boundary at %.3f s%s",
                   (double)s / e->sps, ratio, ph, ok ? "  (usable)" : "");
            if (ok && prev_ok) {
                double slip = fabs(ph - prev_ph);
                if (slip > 0.5) slip = 1.0 - slip;
                printf("  slip %+.3f s vs previous", slip);
                if (slip > worst) { worst = slip; worst_elapsed = (double)s/e->sps-prev_time; }
                nslip++;
            }
            putchar('\n');
            if (ok) { prev_ph = ph; prev_time = (double)s/e->sps; prev_ok = 1; }
        }
        if (nslip) {
            printf("  worst slip between usable windows: %.3f s in %.0f s"
                   " (%.0f ppm equivalent)\n", worst, worst_elapsed, worst / fmax(worst_elapsed,1.0) * 1e6);
            if (worst > 0.05)
                printf("  VERDICT: timing is inconsistent with a fixed second grid;\n"
                       "           capture gaps, fading or interference require independent checking.\n");
        } else {
            printf("  not enough windows with a usable 1 Hz line to check"
                   " continuity\n");
        }
    }
    printf("=======================\n\n");
}

/* ------------------------------------------------------------------ */
/* Runners                                                           */
/* ------------------------------------------------------------------ */

static int run_file(const char *path, unsigned rate, double tone_offset,
                    int manual_offset, int do_analyze, int do_search,
                    int ncand, double at_time, double harm_reject,
                    double min_snr, int verbose, int stream_too, int track_too) {
    pcm_t p;
    memset(&p, 0, sizeof(p));
    if (pcm_load(path, rate, &p) < 0) return 1;

    if (!manual_offset)
        tone_offset = estimate_tone_offset(p.s, p.n, p.rate, 1);
    else
        fprintf(stderr, "using manual tone offset %+.2f Hz\n", tone_offset);

    cenv_t e;
    memset(&e, 0, sizeof(e));
    if (cenv_build(&e, p.s, p.n, p.rate, 100.0 + tone_offset) < 0) {
        fprintf(stderr, "out of memory building code envelope\n");
        pcm_free(&p);
        return 1;
    }

    if (do_analyze) analyze(&p, tone_offset, &e);

    int decoded = 0;
    if (at_time >= 0.0) decoded += decode_at(&e, at_time, min_snr, verbose, NULL);
    if (do_search) decoded += offline_search(&e, ncand, min_snr, verbose);
    if (track_too || (do_search && !decoded)) decoded += offline_track(&e,min_snr,verbose);

    if (stream_too) {
        printf("--- streaming decoder over the same file ---\n");
        decoder_t d;
        decoder_init(&d, p.rate, verbose, tone_offset, harm_reject, min_snr);
        decoder_process(&d, p.s, p.n);
        decoder_flush(&d);
        if (d.harm_blocked)
            fprintf(stderr, "harmonic gate suppressed %" PRIu64
                    " 1000 Hz detections as program-tone distortion\n",
                    d.harm_blocked);
        decoded += (int)d.decoded_frames;
        timing_corr_free(&d.timing);
    }

    cenv_free(&e);
    pcm_free(&p);
    if ((at_time >= 0.0 || do_search || stream_too || track_too) && !decoded) {
        fprintf(stderr,"decode: no complete validated UTC frame (exit status 3)\n");
        return 3;
    }
    return 0;
}

static int run_raw(decoder_t *d, int auto_offset, double harm_reject, double min_snr) {
    int16_t buf[BLOCK_FRAMES];
    if (auto_offset) {
        size_t want = (size_t)d->rate * 12u;
        int16_t *acq = malloc(want * sizeof(*acq));
        if (!acq) { fprintf(stderr, "out of memory\n"); return 1; }
        size_t got = 0;
        while (got < want && !stop_flag) {
            size_t n = fread(acq + got, sizeof(*acq), want - got, stdin);
            got += n;
            if (n == 0) break;
        }
        if (got < (size_t)d->rate * 3u) {
            fprintf(stderr, "auto-offset: need at least 3 seconds of audio\n");
            free(acq);
            return 1;
        }
        double off = estimate_tone_offset(acq, got, d->rate, d->verbose);
        unsigned rate = d->rate;
        int verbose = d->verbose;
        timing_corr_free(&d->timing);
        decoder_init(d, rate, verbose, off, harm_reject, min_snr);
        decoder_process(d, acq, got);
        free(acq);
    }
    while (!stop_flag) {
        size_t n = fread(buf, sizeof(buf[0]), BLOCK_FRAMES, stdin);
        if (n) decoder_process(d, buf, n);
        if (n < BLOCK_FRAMES) {
            if (ferror(stdin)) { perror("stdin"); return 1; }
            break;
        }
    }
    decoder_flush(d);
    return 0;
}

#ifndef WWV_NO_ALSA
static int run_alsa(decoder_t *d, const char *device, int auto_offset,
                    double harm_reject, double min_snr) {
    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, device, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_open(%s): %s\n", device, snd_strerror(err));
        return 1;
    }
    err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED, 1, d->rate, 1, 100000);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_set_params: %s\n", snd_strerror(err));
        snd_pcm_close(pcm);
        return 1;
    }
    fprintf(stderr, "capture: ALSA device=%s rate=%u format=S16_LE mono\n",
            device, d->rate);

    if (auto_offset) {
        size_t want = (size_t)d->rate * 12u;
        int16_t *acq = malloc(want * sizeof(*acq));
        if (!acq) { snd_pcm_close(pcm); return 1; }
        size_t got = 0;
        fprintf(stderr, "auto-offset: acquiring 12 seconds...\n");
        while (got < want && !stop_flag) {
            snd_pcm_sframes_t n = snd_pcm_readi(pcm, acq + got, want - got);
            if (n == -EAGAIN) continue;
            if (n < 0) {
                int r = snd_pcm_recover(pcm, (int)n, 1);
                if (r < 0) {
                    fprintf(stderr, "ALSA error: %s\n", snd_strerror(r));
                    free(acq); snd_pcm_close(pcm); return 1;
                }
                continue;
            }
            got += (size_t)n;
        }
        double off = estimate_tone_offset(acq, got, d->rate, d->verbose);
        unsigned rate = d->rate;
        int verbose = d->verbose;
        timing_corr_free(&d->timing);
        decoder_init(d, rate, verbose, off, harm_reject, min_snr);
        decoder_process(d, acq, got);
        free(acq);
    }

    int16_t buf[BLOCK_FRAMES];
    while (!stop_flag) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm, buf, BLOCK_FRAMES);
        if (n == -EAGAIN) continue;
        if (n < 0) {
            int r = snd_pcm_recover(pcm, (int)n, 1);
            if (r < 0) {
                fprintf(stderr, "ALSA capture error: %s\n", snd_strerror(r));
                snd_pcm_close(pcm);
                return 1;
            }
            continue;
        }
        decoder_process(d, buf, (size_t)n);
    }
    snd_pcm_close(pcm);
    return 0;
}
#endif

static void usage(FILE *f, const char *argv0) {
    fprintf(f,
        "Usage: %s [options]\n"
        "\n"
        "Decode the WWV 100 Hz time code from receiver audio or a recording.\n"
        "\n"
        "Input:\n"
        "  -i, --input FILE   decode a recording (raw S16_LE mono, or WAV)\n"
        "      --raw          read S16_LE mono from stdin\n"
        "  -D, --device DEV   ALSA capture device (default: %s)\n"
        "  -r, --rate HZ      sample rate for raw input (default: %u)\n"
        "\n"
        "Tuning:\n"
        "  -o, --offset HZ    manual audio tone offset\n"
        "  -a, --auto-offset  estimate the offset from the WWV tones\n"
        "      --harm-reject R  a 1000/1500 Hz event must exceed R times the\n"
        "                     500/600 Hz level to count (default %.2f)\n"
        "      --min-snr DB   per-second pulse SNR needed for a bit (default %.1f)\n"
        "\n"
        "File modes (default: analyze and search):\n"
        "      --analyze      print a signal/link-budget report\n"
        "      --search       exhaustive minute-phase search\n"
        "      --candidates N report the best N hypotheses (default 5)\n"
        "      --at SECONDS   decode the frame starting at this file time\n"
        "      --track        follow variable second timing (also a search fallback)\n"
        "      --stream       also run the live streaming decoder over the file\n"
        "      --no-analyze, --no-search\n"
        "  -v, --verbose      per-second DSP diagnostics on stderr\n"
        "  -h, --help         show this help\n"
        "  File decode exits 3 when no complete validated UTC frame is found.\n"
        "\n"
        "Examples:\n"
        "  %s -i real-wwv.raw -r 8000\n"
        "  %s -i capture.wav --at 34.008 -v\n"
        "  %s -D plughw:1,0 -a -v\n"
        "  arecord -D plughw:1,0 -f S16_LE -c1 -r8000 -t raw | %s --raw -a\n",
        argv0, DEFAULT_DEVICE, DEFAULT_RATE, 0.50, 3.0,
        argv0, argv0, argv0, argv0);
#ifdef WWV_NO_ALSA
    fprintf(f, "\nThis build has no ALSA support; use -i or --raw.\n");
#endif
}

enum {
    OPT_RAW = 1000, OPT_ANALYZE, OPT_NOANALYZE, OPT_SEARCH, OPT_NOSEARCH,
    OPT_AT, OPT_CAND, OPT_HARM, OPT_SNR, OPT_STREAM, OPT_TRACK
};

int main(int argc, char **argv) {
    const char *device = DEFAULT_DEVICE;
    const char *infile = NULL;
    unsigned rate = DEFAULT_RATE;
    int raw = 0, verbose = 0, auto_offset = 0, manual_offset = 0;
    double tone_offset = 0.0;
    int do_analyze = -1, do_search = -1, stream_too = 0, track_too = 0;
    int ncand = 5;
    double at_time = -1.0;
    double harm_reject = 0.50;
    double min_snr = 3.0;

    static const struct option opts[] = {
        {"device", required_argument, 0, 'D'},
        {"input", required_argument, 0, 'i'},
        {"rate", required_argument, 0, 'r'},
        {"offset", required_argument, 0, 'o'},
        {"auto-offset", no_argument, 0, 'a'},
        {"raw", no_argument, 0, OPT_RAW},
        {"analyze", no_argument, 0, OPT_ANALYZE},
        {"no-analyze", no_argument, 0, OPT_NOANALYZE},
        {"search", no_argument, 0, OPT_SEARCH},
        {"no-search", no_argument, 0, OPT_NOSEARCH},
        {"at", required_argument, 0, OPT_AT},
        {"candidates", required_argument, 0, OPT_CAND},
        {"harm-reject", required_argument, 0, OPT_HARM},
        {"min-snr", required_argument, 0, OPT_SNR},
        {"stream", no_argument, 0, OPT_STREAM},
        {"track", no_argument, 0, OPT_TRACK},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {0,0,0,0}
    };

    for (;;) {
        int idx = 0;
        int c = getopt_long(argc, argv, "D:i:r:o:avh", opts, &idx);
        if (c == -1) break;
        switch (c) {
            case 'D': device = optarg; break;
            case 'i': infile = optarg; break;
            case 'r': {
                char *end = NULL;
                unsigned long v = strtoul(optarg, &end, 10);
                if (!end || *end || v < 4000 || v > 192000) {
                    fprintf(stderr, "invalid sample rate: %s\n", optarg);
                    return 2;
                }
                rate = (unsigned)v;
                break;
            }
            case 'o': {
                char *end = NULL;
                double v = strtod(optarg, &end);
                if (!end || *end || !isfinite(v) || v < -3000.0 || v > 3000.0) {
                    fprintf(stderr, "invalid tone offset: %s\n", optarg);
                    return 2;
                }
                tone_offset = v;
                manual_offset = 1;
                break;
            }
            case 'a': auto_offset = 1; break;
            case OPT_RAW: raw = 1; break;
            case OPT_ANALYZE: do_analyze = 1; break;
            case OPT_NOANALYZE: do_analyze = 0; break;
            case OPT_SEARCH: do_search = 1; break;
            case OPT_NOSEARCH: do_search = 0; break;
            case OPT_STREAM: stream_too = 1; break;
            case OPT_TRACK: track_too = 1; break;
            case OPT_AT: at_time = strtod(optarg, NULL); break;
            case OPT_CAND: ncand = atoi(optarg); if (ncand < 1) ncand = 1; break;
            case OPT_HARM: harm_reject = strtod(optarg, NULL); break;
            case OPT_SNR: min_snr = strtod(optarg, NULL); break;
            case 'v': verbose++; break;
            case 'h': usage(stdout, argv[0]); return 0;
            default: usage(stderr, argv[0]); return 2;
        }
    }

    if (!infile && optind < argc) infile = argv[optind];

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (infile) {
        if (do_analyze < 0) do_analyze = 1;
        if (do_search < 0) do_search = (at_time < 0.0 && !track_too);
        return run_file(infile, rate, tone_offset, manual_offset, do_analyze,
                        do_search, ncand, at_time, harm_reject, min_snr,
                        verbose, stream_too, track_too);
    }

    if (auto_offset && manual_offset) {
        fprintf(stderr, "note: --offset overrides --auto-offset\n");
        auto_offset = 0;
    }

    decoder_t d;
    decoder_init(&d, rate, verbose, tone_offset, harm_reject, min_snr);
    if (verbose && !auto_offset)
        fprintf(stderr, "detectors: offset=%+.2f Hz data=%.2f tick=%.2f hour=%.2f Hz\n",
                tone_offset, 100.0 + tone_offset, 1000.0 + tone_offset,
                1500.0 + tone_offset);

    int rc;
    if (raw) {
        rc = run_raw(&d, auto_offset, harm_reject, min_snr);
        timing_corr_free(&d.timing);
        return rc;
    }
#ifdef WWV_NO_ALSA
    (void)device;
    fprintf(stderr, "No ALSA support in this build; use -i FILE or --raw.\n");
    timing_corr_free(&d.timing);
    return 2;
#else
    rc = run_alsa(&d, device, auto_offset, harm_reject, min_snr);
    timing_corr_free(&d.timing);
    return rc;
#endif
}
