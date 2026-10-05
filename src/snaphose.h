
#ifndef _snaphose_h
#define _snaphose_h

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// this defines the on-wire data format, which is also needed by the receiver!

#define VER_MAGIC 0x5150

// Used for TCP
enum e_snaphose_ctrl_chars
{
  SNAPHOSE_CTRL_NOISE = 'N',
  SNAPHOSE_CTRL_ANT   = 'A',
  SNAPHOSE_CTRL_TERM  = 'T',
  SNAPHOSE_CTRL_PAUSE = 'P',
  SNAPHOSE_CTRL_DIE =   'Q'
};

enum e_snaphose_source
{

  SNAPHOSE_SRC_UNKNOWN = 'U',
  SNAPHOSE_SRC_ANT = 'A',
  SNAPHOSE_SRC_NOISE = 'N',
  SNAPHOSE_SRC_TERM = 'T'
};


// Could be annoying and use 34/30 like ext4, but we don't need that for a few years
typedef struct snaphose_tm
{
  uint64_t utc_secs : 32;
  uint64_t utc_nsecs : 32;
} snaphost_tm_t;

typedef struct snaphose_data
{
 uint16_t ver_magic;

 uint16_t accum_length;

  // Number of frequency bins
 uint16_t nfreqbins;
 // Number of bits representing each bin
 uint8_t nbits_per_bin;

 // Index of the snap used (0 = 'A')
 uint8_t snap_index;

 // Total number of snaps read
 uint32_t read_counter;

 // Information about firmware
 uint32_t fw_rev;

 // CPU time that this reaodut happened
 snaphost_tm_t readout_cpu_time;

 // CPU time that this readout was sent
 snaphost_tm_t send_cpu_time;

 // CPU time that this readout was received (on RPI)
 snaphost_tm_t rcv_cpu_time;

 // The cycle count at the beginning of this snap
 uint32_t snap_cycle_count;

 // The pps count at time of reading this snap
 //   (could have cycled over)
 uint32_t pps_count_at_read_time;

 // The number of cycles in the last pps
 uint32_t last_pps_duration;

 uint16_t us_elapsed_while_reading; //saturates at 65.5 ms

 struct
 {
    char which; //which source?  (e_snaphose_source)
    uint8_t attenuation; //attenuation for this source (dB? halfdB?)
 } source;


 struct
 {

    //uptimes of various things measured in minutes (up to 45 days, saturating). Unnecessarily saving space here.
    struct
    {
      uint16_t snaphose; //DAQ program
      uint16_t red_pitaya;
      uint16_t rpi;
      uint16_t snapsave; //DAQ saving program
    } uptime;

    // Various RPI things, packed into 64 bits. Sorry Nick.
    struct
    {
      uint64_t disk_free_GiB : 14;
      uint64_t fan_rpm : 14;
      uint64_t rail5V_mV : 13;
      uint64_t throttled : 1;
      uint64_t free_mem_MB : 14;
      int64_t core_temp: 8; //C
    } rpi;

    // Other temperatures (
    struct
    {
      int8_t red_pitaya;
      int8_t w1_probe;
      int8_t cal_board;
      int8_t ssd;
    } temps; //all C. Do we need more precision here, maybe for the cal_board?

  } hsk;


  //for shit we forget
  uint64_t reserved[4];

  //make sure double-word-aligned
  _Alignas(8) uint8_t packed_samples[]; //packed samples, rounded up to nearest cacheline of 64
} snaphose_data_t;

// product gives total in bits, we add 511 the nshift by 9 to convert to number of  cacheliness (512 bits/word) needed, then shift by 6 to get back to bytes
#define SNAPHOSE_PACKED_DATA_SIZE(NUM_BITS, NUM_SAMPLES)   ((((NUM_BITS * NUM_SAMPLES) + 511) >> 9) << 6)
#define SNAPHOSE_DATA_SIZE_NEEDED(NUM_BITS, NUM_SAMPLES)  sizeof(snaphose_data_t) + SNAPHOSE_PACKED_DATA_SIZE(NUM_BITS, NUM_SAMPLES)


int snaphose_dump(FILE * f, const snaphose_data_t * s);


static inline uint64_t snaphose_nth_sample_u64(const snaphose_data_t *s, size_t i)
{
  if (i >= s->nfreqbins)  return (uint64_t) -1;

  switch (s->nbits_per_bin)
  {
    case 64:
      return (((uint64_t*) s->packed_samples)[i]);
    case 32:
      return (((uint32_t*) s->packed_samples)[i]);
    case 16:
      return (((uint16_t*) s->packed_samples)[i]);
    case 8:
      return s->packed_samples[i];
    default:
      break;
  }
  // If we ever ahave nbits_per_in not 64, we should check/optimize this
  //
  uint64_t val = 0;


  size_t bin = (i * s->nbits_per_bin) >>6;
  uint8_t start_bit = (i * s->nbits_per_bin) % 64;
  uint8_t bits_first_double_word = (s->nbits_per_bin - start_bit) % 64;
  uint8_t bits_second_double_word = bits_first_double_word == s->nbits_per_bin ? 0 : s->nbits_per_bin - bits_first_double_word;
  val = (((uint64_t*) s->packed_samples)[bin] >> start_bit); 
  val &= (bits_first_double_word ==64) ?  UINT64_MAX :  ( 1 << bits_first_double_word) -1;

  if (bits_second_double_word)
    val += (((uint64_t*) s->packed_samples)[bin] & ((1 << bits_second_double_word)-1)) << bits_first_double_word;

  val |= (1 << bits_second_double_word) -1;

  return (val);
}

/** Unpack samples to a 64-bit array */
int snaphose_unpack_samples_u64(const snaphose_data_t * s,  uint32_t dest_sz,  uint64_t  * dest);

/** Unpack samples to a 32-bit, truncating if needed */
int snaphose_unpack_samples_u32(const snaphose_data_t * s,  uint32_t dest_sz,  uint64_t  * dest);

/** Unpack samples to a 32-bit float*/
int snaphose_unpack_samples_f32(const snaphose_data_t * s,  uint32_t dest_sz,  float  * dest);

/** Unpack samples to a 64-bit double */
int snaphose_unpack_samples_f64(const snaphose_data_t * s ,  uint32_t dest_sz,  double  * dest);


#ifdef __cplusplus
}
#endif

#endif

