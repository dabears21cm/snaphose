#ifndef SONIFY_H
#define SONIFY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sonifier sonifier;

typedef struct {
    int      nb;              /* bins per spectrum                                 */
    double   fs;              /* audio sample rate (Hz)                            */
    double   frame_dt;        /* spectrum cadence (s), fractional spf is fine      */
    double   fmin, fmax;      /* audio band the bins are mapped onto (Hz)          */
    float    gamma;           /* amp = excess^(gamma/2); 1 = plain sqrt(power)     */
    float    gain;            /* pre-tanh gain; 0 -> 1/sqrt(nb)                    */
    double   ema_tau;         /* baseline EMA time constant (s); 0 = fixed         */
    double   agc_tau;         /* time constant for AGC  */
    float    target_rms ;     /* pre-tanh RMS wanted,  */
    int      subtract_floor;  /* 1: use (S/base - 1)+, 0: use S/base if have base   */
    unsigned long long seed;  /* initial-phase seed                                 */
} sonify_params;

#define SONIFY_DEFAULT_PARAMS  .nb = 1024, .fs = 48e3, . frame_dt = 1e-3, .fmin = 0, .fmax = 10000.0, .gamma = 0.5, .gain = 0, .ema_tau = 0,  .subtract_floor = 1, .seed = 4 , .agc_tau = 30, .target_rms = 0.15

/* baseline may be NULL: then the baseline is zero. */
sonifier *sonify_create(const sonify_params *p, const float *baseline);
void      sonify_destroy(sonifier *s);

/* Replace the baseline (nb linear-power values). */
void      sonify_set_baseline(sonifier *s, const float *baseline);

/* Upper bound on samples returned by one sonify_push(). */
size_t    sonify_max_samples(const sonifier *s);

/* Push one linear-power spectrum (nb floats). Writes audio in [-1, 1] to out
 * (at least sonify_max_samples() floats), returns the number of samples. */
size_t    sonify_push(sonifier *s, const float *spectrum, float *out);

#ifdef __cplusplus
}
#endif
#endif
