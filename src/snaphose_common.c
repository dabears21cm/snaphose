#include "snaphose.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

int snaphose_dump(FILE * f, const snaphose_data_t * s)
{

  int out = 0;

  out += fprintf(f, " (snaphose_data_t) { .nfreqbins = %hu, .nbits_per_bin = %hhu, .snap_index = %hhu, .read_counter = %u\n, ", s->nfreqbins, s->nbits_per_bin, s->snap_index, s->read_counter);
  out += fprintf(f, "                     .sys_board_id = %x, .sys_rev = %x, .readout_cpu_time = { .utc_secs = %"PRIu64", .utc_nsecs = %"PRIu64" }, \n", s->sys_board_id, s->sys_rev, (uint64_t) s->readout_cpu_time.utc_secs, (uint64_t) s->readout_cpu_time.utc_nsecs);
  out += fprintf(f, "                     .send_cpu_time = { .utc_secs = %"PRIu64", .utc_nsecs = %"PRIu64" }, .rcv_cpu_time = {.utc_secs = %"PRIu64", .utc_nsecs = %" PRIu64 "}, \n", (uint64_t)  s->send_cpu_time.utc_secs, (uint64_t) s->send_cpu_time.utc_nsecs,(uint64_t)  s->rcv_cpu_time.utc_secs, (uint64_t) s->rcv_cpu_time.utc_nsecs);
  out += fprintf(f, "                     .snap_cycle_count = %u, .pps_count_at_read_time =%u, .last_pps_at_read_time = %u, .last_pps_duration=%u, \n", s->snap_cycle_count, s->pps_count_at_read_time, s->last_pps_at_read_time, s->last_pps_duration);
  out += fprintf(f, "                     .us_elapsed_while_reading = %hu, .source = { .which =  '%c',  .attenuation = %hhu},\n", s->us_elapsed_while_reading, s->source.which, s->source.attenuation);
  out += fprintf(f, "                     .hsk = {  .uptime = { .snaphose = %hu, .red_pitaya = %hu, .rpi = %hu, .snapsave = %hu},\n", s->hsk.uptime.snaphose, s->hsk.uptime.red_pitaya, s->hsk.uptime.rpi, s->hsk.uptime.snapsave);
  out += fprintf(f, "                               .rpi = { .disk_free_GiB = %"PRIu64", .fan_rpm = %"PRIu64", .rail5V_mV = %"PRIu64", .throttled = %"PRIu64", .free_mem_MB = %"PRIu64", .core_temp = %"PRIu64"},\n", (uint64_t) s->hsk.rpi.disk_free_GiB,(uint64_t)  s->hsk.rpi.fan_rpm,(uint64_t)  s->hsk.rpi.rail5V_mV,(uint64_t)  s->hsk.rpi.throttled,(uint64_t)  s->hsk.rpi.free_mem_MB,(uint64_t)  s->hsk.rpi.core_temp);
  out += fprintf(f, "                               .temps = { .red_pitaya = %hhd, .w1_probe = %hhd, .cal_board = %hhd, .ssd = %hhd},\n", s->hsk.temps.red_pitaya, s->hsk.temps.w1_probe, s->hsk.temps.cal_board, s->hsk.temps.ssd);
  out += fprintf(f, "                             },\n");
  out += fprintf(f, "                     .samples_dB = {\n");
  out += fprintf(f, "                        ");

  for (int i = 0; i < s->nfreqbins; i++)
  {
    uint64_t val = snaphose_nth_sample_u64(s,i);
    double dB = 10 * log10(val);
    out += fprintf(f, "%0.3f,", dB);
    if (i % 8 == 0) out += fprintf(f, "\n                        ");
  }

  out += fprintf(f, "                               }\n");
  out += fprintf(f, "                  };\n");
  return out;
}



/** Unpack samples to a 64-bit array */
int snaphose_unpack_samples_u64(const snaphose_data_t * s,  uint32_t dest_sz,  uint64_t  * dest)
{
  if (s->nbits_per_bin == 64)
  {
    memcpy(dest, s->packed_samples,  dest_sz*sizeof(uint64_t));
  }
  else
  {
    for (uint32_t i = 0; i < dest_sz; i++)
    {
      dest[i] = snaphose_nth_sample_u64(s,i);
    }
  }


  return 0;
}

// use simd into float conversions in the happy case


#if defined(__AVX512F__)
    #define SIMD_BYTES 64
#elif defined(__AVX2__) || defined(__AVX__)
    #define SIMD_BYTES 32
#else
    #define SIMD_BYTES 16  // neon/sse etc.
#endif


// the half vecs are for when the int size doesn't match the float size (u64->float or u32->double)
typedef uint64_t u64_vec   __attribute__ ((vector_size (SIMD_BYTES)));
typedef uint32_t u32_vec   __attribute__ ((vector_size (SIMD_BYTES)));
typedef double double_vec   __attribute__ ((vector_size (SIMD_BYTES)));
typedef float  float_vec   __attribute__ ((vector_size (SIMD_BYTES)));
typedef float  float_half_vec   __attribute__ ((vector_size (SIMD_BYTES/2)));
typedef uint32_t u32_half_vec   __attribute__ ((vector_size (SIMD_BYTES/2)));

/** Unpack samples to a 32-bit float*/
int snaphose_unpack_samples_f32(const snaphose_data_t * s,  uint32_t dest_sz,  float  * dest)
{

  if (s->nbits_per_bin == 64)
  {
    uint32_t i = 0;
    uint64_t * as_u64 = (uint64_t *) s->packed_samples;

#pragma GCC unroll 4
    for (; i <= dest_sz; i+=  (SIMD_BYTES / 8))
    {
      u64_vec chunk;
      __builtin_memcpy(&chunk, &as_u64[i], sizeof(u64_vec));
      float_half_vec result = __builtin_convertvector(chunk, float_half_vec);
      __builtin_memcpy(&dest[i], &result, sizeof(float_half_vec));
    }

    //cleanup loop
    for (; i < dest_sz; i++) dest[i] = (float) as_u64[i];
  }
  else if (s->nbits_per_bin == 32)
  {
    uint32_t i = 0;
    uint32_t * as_u32 = (uint32_t *) s->packed_samples;

#pragma GCC unroll 4
    for (; i <= dest_sz; i+=  (SIMD_BYTES / 4))
    {
      u32_vec chunk;
      __builtin_memcpy(&chunk, &as_u32[i], sizeof(u32_vec));
      float_vec result = __builtin_convertvector(chunk, float_vec);
      __builtin_memcpy(&dest[i], &result, sizeof(float_vec));
    }

    //cleanup loop
    for (; i < dest_sz; i++) dest[i] = (float) as_u32[i];
  }
  else
  {
    // fuck it, hope for the best
    for (uint32_t i = 0; i < dest_sz; i++) dest[i] = (float) snaphose_nth_sample_u64(s,i);

  }

  return 0;
}

/** Unpack samples to a 64-bit double */
int snaphose_unpack_samples_f64(const snaphose_data_t * s ,  uint32_t dest_sz,  double  * dest)
{
  if (s->nbits_per_bin == 64)
  {
    uint32_t i = 0;
    uint64_t * as_u64 = (uint64_t *) s->packed_samples;

#pragma GCC unroll 4
    for (; i <= dest_sz; i+=  (SIMD_BYTES / 8))
    {
      u64_vec chunk;
      __builtin_memcpy(&chunk, &as_u64[i], sizeof(u64_vec));
      double_vec result = __builtin_convertvector(chunk, double_vec);
      __builtin_memcpy(&dest[i], &result, sizeof(double_vec));
    }

    //cleanup loop
    for (; i < dest_sz; i++) dest[i] = (double) as_u64[i];
  }
  else if (s->nbits_per_bin == 32)
  {
    uint32_t i = 0;
    uint32_t * as_u32 = (uint32_t *) s->packed_samples;

#pragma GCC unroll 4
    for (; i <= dest_sz; i+=  (SIMD_BYTES / 4))
    {
      u32_half_vec chunk;
      __builtin_memcpy(&chunk, &as_u32[i], sizeof(u32_half_vec));
      double_vec result = __builtin_convertvector(chunk, double_vec);
      __builtin_memcpy(&dest[i], &result, sizeof(double_vec));
    }

    //cleanup loop
    for (; i < dest_sz; i++) dest[i] = (double) as_u32[i];
  }
  else
  {
    // fuck it, hope for the best
    for (uint32_t i = 0; i < dest_sz; i++) dest[i] = (double) snaphose_nth_sample_u64(s,i);
  }

  return 0;
}
