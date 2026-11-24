#include "sonify.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TWO_PI 6.283185307179586

struct sonifier {
    int      nb, stride;
    double   spf;
    float    gamma, gain, ema_alpha;
    float    agc_alpha, target_rms, ms_avg;
    int ms_init;
    int      subtract_floor;
    float   *zr, *zi, *rr, *ri, *a0, *da, *base;
    uint64_t m, n;
    float   *block;
    int using_base;
};

static uint64_t splitmix64(uint64_t *x)
{
    uint64_t z = (*x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

sonifier *sonify_create(const sonify_params *p, const float *baseline)
{
    if (!p || p->nb <= 0 || p->fs <= 0 || p->frame_dt <= 0 ||
        p->fmin < 0 || p->fmax <= p->fmin || p->fmax > 0.5 * p->fs)
        return NULL;

    sonifier *s = calloc(1, sizeof *s);
    if (!s) return NULL;

    s->nb     = p->nb;
    s->stride = (p->nb + 15) & ~15;                 /* 64-byte multiples */
    s->block  = aligned_alloc(64, 7 * (size_t)s->stride * sizeof(float));
    if (!s->block) { free(s); return NULL; }
    memset(s->block, 0, 7 * (size_t)s->stride * sizeof(float));

    float *b = s->block;
    s->zr = b;  b += s->stride;   s->zi = b;  b += s->stride;
    s->rr = b;  b += s->stride;   s->ri = b;  b += s->stride;
    s->a0 = b;  b += s->stride;   s->da = b;  b += s->stride;
    s->base = b;

    s->spf            = p->fs * p->frame_dt;
    s->gamma          = p->gamma;
    s->gain           = p->gain > 0 ? p->gain : 1.0f / sqrtf((float)p->nb);
    s->ema_alpha      = p->ema_tau > 0 ? (float)(p->frame_dt / p->ema_tau) : 0.f;
    s->subtract_floor = p->subtract_floor;

    uint64_t st = p->seed;
    for (int k = 0; k < s->nb; k++) {
        double f  = p->fmin + (k + 0.5) / s->nb * (p->fmax - p->fmin);
        double w  = TWO_PI * f / p->fs;
        double ph = TWO_PI * (double)(splitmix64(&st) >> 11) * 0x1.0p-53;
        s->rr[k] = (float)cos(w);  s->ri[k] = (float)sin(w);
        s->zr[k] = (float)cos(ph); s->zi[k] = (float)sin(ph);
    }

    s->agc_alpha  = p->agc_tau > 0 ? (float)(p->frame_dt / p->agc_tau) : 0.f;
    s->target_rms = p->target_rms > 0 ? p->target_rms : 0.15f;
    s->ms_init    = 0;
    if (baseline) sonify_set_baseline(s, baseline);
    return s;
}

void sonify_destroy(sonifier *s)
{
    if (!s) return;
    free(s->block);
    free(s);
}

void sonify_set_baseline(sonifier *s, const float *baseline)
{
  if (baseline)
  {
    memcpy(s->base, baseline, (size_t)s->nb * sizeof(float));
    s->using_base = 1;
  }
  else
  {
    memset(s->base, 0, (size_t)  s->nb * sizeof(float));
    s->using_base = 0;
  }
}

size_t sonify_max_samples(const sonifier *s)
{
    return (size_t)ceil(s->spf) + 1;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-use-of-uninitialized-value"
size_t sonify_push(sonifier *s, const float *S, float *out)
{
    const int nb = s->nb;
    float *restrict zr = s->zr, *restrict zi = s->zi;
    const float *restrict rr = s->rr, *restrict ri = s->ri;
    float *restrict a0 = s->a0, *restrict da = s->da, *restrict base = s->base;

    if (s->using_base && s->ema_alpha > 0) {
        for (int k = 0; k < nb; k++)
            base[k] += s->ema_alpha * (S[k] - base[k]);
    }

    const float off = s->subtract_floor ? 1.f : 0.f;
    for (int k = 0; k < nb; k++) {
        float r = s->using_base ?  (base[k] > 0 ? S[k] / base[k] - off : 0.0f )
                                : S[k];
        if (!(r > 0)) r = 0;                                /* also NaN */
        da[k] = powf(r, 0.5f * s->gamma) - a0[k];
    }


    if (s->agc_alpha > 0) {
      float ms = 0.f;
      for (int k = 0; k < nb; k++) {
        float a = a0[k] + da[k];          /* this frame's target amplitude */
        ms += a * a;
      }
      ms *= 0.5f;
      if (!s->ms_init)
      {
        s->ms_avg = ms;
        s->ms_init = 1;
      }
      else
      {
        s->ms_avg += s->agc_alpha * (ms - s->ms_avg);
      }

      if (s->ms_avg > 0)
      {
        s->gain = s->target_rms / sqrtf(s->ms_avg);
      }
    }

    /* samples n with m*spf <= n < (m+1)*spf; amplitude ramps a0 -> a0+da */
    const double   t0  = (double)s->m * s->spf;
    const uint64_t n1  = (uint64_t)ceil((double)(s->m + 1) * s->spf);
    const float    inv = (float)(1.0 / s->spf);
    size_t j = 0;

    for (uint64_t n = s->n; n < n1; n++, j++) {
        const float x = (float)((double)n - t0) * inv;
        float acc = 0.f;
        #pragma omp simd reduction(+:acc)
        for (int k = 0; k < nb; k++) {
            float r = zr[k] * rr[k] - zi[k] * ri[k];
            float i = zr[k] * ri[k] + zi[k] * rr[k];
            zr[k] = r;
            zi[k] = i;
            acc += (a0[k] + da[k] * x) * i;
        }
        out[j] = tanhf(s->gain * acc);
    }

    for (int k = 0; k < nb; k++) {
        float g = 1.5f - 0.5f * (zr[k] * zr[k] + zi[k] * zi[k]);
        zr[k] *= g;
        zi[k] *= g;
        a0[k] += da[k];
    }

    s->n = n1;
    s->m++;
    return j;
}
#pragma GCC diagnostic pop
