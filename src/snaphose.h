
#ifndef _snaphose_h
#define _snaphose_h

#include <stdint.h>
#include <stdio.h>

// this defines the on-wire data format, which is also needed by the receiver!


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
  // Number of frequency bins
 uint16_t nfreqbins;
 // Number of bits representing each bin
 uint8_t nbits_per_bin;

 // Index of the snap used (0 = 'A') 
 uint8_t snap_index;

 // Total number of snaps read
 uint32_t read_counter;


 // Information about firmware
 uint32_t sys_board_id;
 uint32_t sys_rev;

 // CPU time that this reaodut happened
 snaphost_tm_t readout_cputime;

 // CPU time that this readout was sent
 snaphost_tm_t send_cpu_time;

 // CPU time that this readout was received (on RPI)
 snaphost_tm_t rcv_cpu_time;

 // The cycle count at the beginning of this snap
 uint32_t snap_cycle_count;

 // The pps count at time of reading this snap
 uint32_t pps_count_at_read_time;

 // The time of the last pps at read time (could be NEXT pps if greater than cycle count)
 uint32_t last_pps_at_read_time;

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

  //make sure word-aligned
  _Alignas(4) uint8_t packed_samples[]; //packed samples, rounded up to nearest multiple of 64
} snaphose_data_t;

// product gives total in bits, we add 63 the nshift by 9 to convert to number of  64-bit words (512 bits/word) needed, then shift by 6 to get back to bytes
#define SNAPHOSE_PACKED_DATA_SIZE(NUM_BITS, NUM_SAMPLES)   ((((NUM_BITS * NUM_SAMPLES) + 63) >> 9) << 6)
#define SNAPHOSE_DATA_SIZE_NEEDED(NUM_BITS, NUM_SAMPLES)  sizeof(snaphose_data_t) + SNAPHOSE_PACKED_DATA_SIZE(NUM_BITS, NUM_SAMPLES)


int snaphose_dump(FILE * f, const snaphose_data_t * s);

/** Unpack samples to a 64-bit array */
int snaphose_unpack_samples_u64(const snaphose_data_t * s,  uint32_t dest_sz,  uint64_t  * dest);

/** Unpack samples to a 32-bit, truncating if needed */
int snaphose_unpack_samples_u32(const snaphose_data_t * s,  uint32_t dest_sz,  uint64_t  * dest);

/** Unpack samples to a 32-bit float*/
int snaphose_unpack_samples_f32(const snaphose_data_t * s,  uint32_t dest_sz,  float  * dest);

/** Unpack samples to a 64-bit double */
int snaphose_unpack_samples_f64(const snaphose_data_t * s ,  uint32_t dest_sz,  double  * dest);


#endif
