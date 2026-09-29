#  include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <stdatomic.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <systemd/sd-daemon.h>
#include <sys/types.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "snaphose.h"


#include "./ini.h"

// Can be modified at compiletime with -DMAX_SNAPS=N
#ifndef MAX_SNAPS
#define MAX_SNAPS 32
#endif


struct snap_setup
{
  uint32_t ctrl;
  uint32_t status;
  uint32_t bram;
  uint32_t val;
  uint32_t ctrl_flags;  // 0 for now, I think?
  size_t captures_since_drain; // captures from last drain
} setup[MAX_SNAPS];


static size_t nblocks = 0;

#define FPGA_MEM_SIZE  (32 *1024*1024)
#define FPGA_MEM_BASE  0x40000000


#define SNAP_CTRL_ENABLE 1
#define SNAP_STATUS_DONE  (1u << 31)


//fpga mem will be mmaped here
static volatile char * fpga;

static int write_reg(uint32_t address, uint32_t val)
{
  if (!fpga) return -1;

   volatile uint32_t * ptr =  (volatile uint32_t*)  ( &fpga[address]);
   *ptr = val;
   return 0;
}


static int read_reg(uint32_t address, uint32_t * val)
{
  if (!fpga) return -1;
   volatile uint32_t * ptr =  (volatile uint32_t*)  ( &fpga[address]);
   *val = *ptr;
   return 0;
}


// configuration options, read from file, or possibly overwritten by command line in a few cases
static short dest_port;
static const char * dest_addr = "255.255.255.255";
static bool udp_broadcast;
static short tcp_control_port;
static bool verbose;
static uint32_t nsamples = 2048;
static uint32_t data_bits = 64;
static uint32_t buffer_size = 256;
static uint32_t buffer_size_mask = 0xff;
static int watchdog_interval = 10;


static struct
{
  uint32_t accum_len;
  uint32_t valid_counts;
  uint32_t total_counts;
  uint32_t clk_cnts;
  uint32_t pps_duration;
  uint32_t pps_counter;
  uint32_t toggle;
} reg;

const char * setup_file = "/etc/snaphose.ini" ;


static int boolish(const char * s, bool * result)
{
  if (!strcmp(s,"true") || !strcmp(s,"yes") || !strcmp(s,"y")) *result = true;
  else if (!strcmp(s,"false") || !strcmp(s,"no") || !strcmp(s,"n")) *result = false;
  else
  {
    fprintf(stderr,"Found non-boolish string %s\n", s);
    return 1;
  }
  return 0;
}

static int setup_handler(void * user, const char * section, const char * name, const char * val)
{
  (void) user;

  if (!strcmp(section,"net"))
  {
    if (!strcmp(name,"udp_port"))
    {
      int maybe_port = atoi(val);
      if (maybe_port > 0&& maybe_port < 65536)
        dest_port = maybe_port;
      else
        fprintf(stderr," Warning: invalid udp port specified, using default.\n");
    }
    else if (!strcmp(name,"tcp_control_port"))
    {
      int maybe_port = atoi(val);
      if (maybe_port > 0&& maybe_port < 65536)
        tcp_control_port = maybe_port;
      else
        fprintf(stderr," Warning: invalid tcp port specified, using default.\n");
    }
    else if (!strcmp(name,"udp_dest"))
    {
      dest_addr = strdup(val);
    }
    else if (!strcmp(name,"udp_bcast"))
    {
      return !boolish(val, &udp_broadcast);
    }
    else
    {
      fprintf(stderr, "Unknown key %s in %s\n", name, section);
      return 0;
    }
  }

#define PARSE_ADDR(what, dest) \
  if (!strcmp(name,what)) {\
    char * endptr = 0;\
    uint32_t maybe = strtoul(val, &endptr, 0);\
    if (!*endptr && ((maybe & 0x3)==0) &&  maybe >= FPGA_MEM_BASE && maybe < FPGA_MEM_SIZE + FPGA_MEM_BASE ) {dest = maybe - FPGA_MEM_BASE; }\
    else { fprintf(stderr, "Bad value %s (0x%x) for %s.%s (endptr=%s)\n", val, maybe, section, name,endptr);  return 1;}\
  }

  else if (!strcmp(section, "reg"))
  {

#define PARSE_REG(X) PARSE_ADDR(#X, reg.X)

    PARSE_REG(accum_len)
    else PARSE_REG(valid_counts)
    else PARSE_REG(total_counts)
    else PARSE_REG(clk_cnts)
    else PARSE_REG(pps_duration)
    else PARSE_REG(pps_counter)
    else PARSE_REG(toggle)
    else
    {
      fprintf(stderr, "Unknown key %s.%s\n", section,name);
      return 0;
    }
  }

  else if (!strcmp(section, "general"))
  {
    if (!strcmp(name,"verbose")) return !boolish(val, &verbose);
    else if (!strcmp(name,"watchdog")) watchdog_interval = atoi(val);
    else
    {
      fprintf(stderr, "Unknown key %s.%s\n", section,name);
      return 0;
    }
  }

  else if (strstr(section,"snap"))
  {
    int snap_id = -1;
    char garbage;
    if (1 == sscanf(section,"snap.%d%s", &snap_id, &garbage))
    {
      if (snap_id < 0 || snap_id >= MAX_SNAPS)
      {
        fprintf(stderr,"Invalid snapid in %s\n", section);
        return 0;
      }

#define PARSE_SNAP(X) PARSE_ADDR(#X, setup[snap_id].X)

      PARSE_SNAP(ctrl)
      else PARSE_SNAP(status)
      else PARSE_SNAP(bram)
      else PARSE_SNAP(val)
      else
      {
        fprintf(stderr,"Invalid key %s in %s\n", name, section);
      }
    }
    else
    {
       fprintf(stderr,"Malformed section title %s\n", section);
       return 0;
    }
  }
  else if (strstr(section,"data"))
  {
    if (!strcmp(name,"addr_width")) nsamples = 1 << atoi(val); 
    else if (!strcmp(name,"data_width")) data_bits = atoi(val);
    else 
    {
      fprintf(stderr,"Invalid key %s in %s\n", name, section);
      return 0;
    }
  }
  else
  {
    fprintf(stderr,"Unknown section/key %s.%s\n", section,name);
    return 0;
  }

  return 1;
}

static int read_setup_file( const char * s)
{
  FILE * f = fopen(s,"r");

  if (!f)
  {
    fprintf(stderr, "Could not open %s. You can provide an alternative setup file via -f. \n", s);
    return 1;
  }

  int ini_ret = ini_parse_file(f, setup_handler, NULL);

  fclose(f);
  if (ini_ret)
  {
    fprintf(stderr,"Trouble parsing config (%d)\n", ini_ret);
    return 1;
  }


  return 0;

}



static pthread_t the_ctrl_thread;
static pthread_t the_tx_thread;
static pthread_t the_read_thread;

static void * ctrl_thread(void*);
static void * tx_thread(void*);
static void * read_thread(void*);


enum e_state
{
  SNAPHOSE_INIT,
  SNAPHOSE_PAUSE,
  SNAPHOSE_RUN,
  SNAPHOSE_DIE,  //anything after this is fatal
  SNAPHOSE_FAIL
};


// Shared stuff
static _Atomic int state = SNAPHOSE_INIT;
static _Atomic int source = SNAPHOSE_SRC_UNKNOWN;
static _Atomic bool drain = true; //  Toggle on whether or not we need to drain

static struct timespec program_start;

static _Alignas(64) _Atomic size_t buffer_written_shared;
static size_t buffer_elem_sz;
static bool got_mem;
// the buffer
char * buffer = 0;
static _Alignas(64) _Atomic size_t buffer_read_shared;

static void buffer_init()
{
  buffer_elem_sz = SNAPHOSE_DATA_SIZE_NEEDED(data_bits, nsamples);
  buffer = calloc(buffer_size, buffer_elem_sz);
}

static snaphose_data_t * buffer_retrieve()
{

  size_t current_written, current_read;

  current_written = atomic_load_explicit(&buffer_written_shared, memory_order_acquire);
  current_read = atomic_load_explicit(&buffer_written_shared, memory_order_relaxed);
  if (current_read == current_written) return NULL;

  size_t idx = current_read & buffer_size_mask;
  return (snaphose_data_t*) &buffer[idx * buffer_elem_sz];
}

static snaphose_data_t * buffer_acquire()
{

  size_t current_written, current_read;
  while (true)
  {
    current_written = atomic_load_explicit(&buffer_written_shared, memory_order_relaxed);
    current_read = atomic_load_explicit(&buffer_written_shared, memory_order_acquire);

    if (current_written-current_read < buffer_size)
    {
      break;
    }
    else
    {
      fprintf(stderr,"WARNING BUFFER IS FULL");
      usleep(100);
    }
  }
  size_t idx = current_written & buffer_size_mask;
  got_mem = true;
  return (snaphose_data_t*) &buffer[idx * buffer_elem_sz];
}

static void buffer_commit()
{
  if (!got_mem) return;
  atomic_fetch_add_explicit(&buffer_written_shared, 1, memory_order_release);
  got_mem = false;
}

static void buffer_drop()
{

  size_t current_written = atomic_load_explicit(&buffer_written_shared, memory_order_acquire);
  size_t current_read = atomic_load_explicit(&buffer_written_shared, memory_order_relaxed);

  // nothing to dropp
  if (current_written == current_read) return;

  atomic_fetch_add_explicit(&buffer_read_shared, 1, memory_order_release);
}

int main(int nargs, char ** args)
{

  clock_gettime(CLOCK_MONOTONIC,&program_start);
  //parse arguments
  for (int iarg = 1; iarg < nargs; iarg++)
  {
    if (!strcmp(args[iarg],"-f")  && iarg < nargs-1)
    {
      setup_file = args[++iarg];
    }
    else
    {
      fprintf(stderr,"Usage: snaphose [-f config-file = /etc/snaphose.ini]\n");
      return 1;
    }
  }

  //parse setup file
  if (read_setup_file(setup_file))
  {
    return 1;
  }

  //validate regs
  if (!reg.accum_len || !reg.valid_counts || !reg.total_counts || !reg.clk_cnts || !reg.pps_duration || !reg.pps_counter || !reg.toggle || !reg.clk_cnts)
  {
    fprintf(stderr,"ruhroh, one or more regs not defined. It would be nice if I told you which one(s) wouldn't it?\n");
    return 1;
  }


  //validate snap setups
  for (int i = 0 ; i < MAX_SNAPS; i++)
  {
    if (setup[i].ctrl || setup[i].status || setup[i].bram || setup[i].val)
    {
      printf("snap.%d { 0x%x 0x%x 0x%x 0x%x } \n", i, setup[i].ctrl, setup[i].status, setup[i].bram, setup[i].val);
      if (nblocks != i)
      {
        fprintf(stderr,"ruhroh, non-continuous snap definitions detected. Ignoring after first empty one\n");
        return 1;
      }
      else
      {
        nblocks++;
      }
    }
  }

  if (!nblocks)
  {
    fprintf(stderr,"No snap blocks defined\n");
  }
  else
  {
    printf("Using %zu snap blocks\n", nblocks);
  }


  int ret = 0;

  //mmap the FPGA

  int dev_mem_fd = open("/dev/men", O_RDWR | O_SYNC);
  if (!dev_mem_fd)
  {
    fprintf(stderr,"Failed to open /dev/mem\n");
    ret = 1;
    goto cleanup;
  }

  errno = 0;
  fpga = (volatile char *) mmap(NULL, FPGA_MEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, dev_mem_fd, FPGA_MEM_BASE);

  if (!fpga || errno)
  {
    fprintf(stderr,"Failed to mmap FPGA (%d)\n", errno);
    ret = 1;
    goto cleanup;
  }

  // open up network ports
  int ctrl_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (ctrl_fd <= 0)
  {
    fprintf(stderr, "Could not open TCP socket\n");
    ret = 1;
    goto cleanup;
  }

  int tx_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (tx_fd <= 0)
  {
    fprintf(stderr, "Could not open UDP socket\n");
    ret = 1;
    goto cleanup;
  }


  //allow reuse
  int opt = 1;
  if (setsockopt(ctrl_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) || setsockopt(tx_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
  {
    fprintf(stderr,"SO_REUSEADDR FAIL\n");
    ret = 1;
    goto cleanup;
  }

  if (udp_broadcast)
  {
    if (setsockopt(tx_fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt)))
    {
      fprintf(stderr,"cannot broadcast\n");
      ret = 1;
      goto cleanup;
    }
  }


  // bind ctrl socket 
  struct sockaddr_in  listen_addr = { 
                                     .sin_family = AF_INET, 
                                     .sin_addr = { .s_addr = INADDR_ANY}, 
                                     .sin_port = htons(tcp_control_port) 
                  };

  if (bind(ctrl_fd, (struct sockaddr*) &listen_addr, sizeof(listen_addr)) < 0)
  {
    fprintf(stderr,"Failed to bind\n");
    ret = 1;
    goto cleanup;
  }

  //enable listening on control socket
  if (listen(ctrl_fd, 5) < 0)
  {
    fprintf(stderr,"Listening fail\n");
    ret = 1; 
    goto cleanup;
  }


  struct sockaddr_in dest_address = {
                                      .sin_family = AF_INET,
                                      .sin_addr = { .s_addr = inet_addr(dest_addr)},
                                      .sin_port = htons(dest_port)
  };

  if (connect(tx_fd, (struct sockaddr*) &dest_address, sizeof(dest_address)))
  {
    fprintf(stderr,"connect failed\n");
    ret = 1;
    goto cleanup;
  }


  buffer_init();

  if (!buffer) 
  {
    fprintf(stderr,"Could not allocate buffer\n");
    ret = 1;
    goto cleanup;
  }

  //start the threads


  if ( pthread_create(&the_ctrl_thread, NULL, ctrl_thread, &ctrl_fd) ||
       pthread_create(&the_tx_thread, NULL, tx_thread, &tx_fd) ||
       pthread_create(&the_read_thread, NULL, read_thread, NULL))
  {

    fprintf(stderr,"Thread starting failed\n");
    ret = 1;
    goto cleanup;
  }



  //notify that we succesfully initialized
  sd_notify(0,"READY=1");


  struct timespec last_watchdog = {0};
  while(state < SNAPHOSE_DIE)
  {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - last_watchdog.tv_sec > watchdog_interval)
    {
      sd_notify(0, "WATCHDOG=1");
      memcpy(&last_watchdog, &now, sizeof(now));
    }
    sleep(1);
  }


  pthread_join(the_read_thread, NULL);
  pthread_join(the_tx_thread, NULL);
  pthread_join(the_ctrl_thread, NULL);

cleanup:
  if (fpga) munmap((void*)fpga,0);
  if (dev_mem_fd) close(dev_mem_fd);
  if (ctrl_fd) close(ctrl_fd);
  if (tx_fd) close(tx_fd);
  if (buffer) free(buffer);


  return ret;
}

struct
{
  float core_temp_iio0;
  float core_temp_iio1;
} hsk;

#define IIO_PATH_PREFIX  "/sys/bus/iio/devices/iio:device"
#define HSK_PATH(WHAT,IIODEV,SUFFIX) IIO_PATH_PREFIX #IIODEV "/in_" #WHAT #SUFFIX

#define GEN_HSK(WHAT,II0DEV)\
static float hsk_get_##WHAT##_iio##II0DEV() {\
    uint32_t raw=0; uint32_t offset=0; float scale=1; \
    FILE* f  = fopen(HSK_PATH(WHAT,IIODEV,_raw),"r");\
    if (!f) { fprintf(stderr,"Could not open "  HSK_PATH(WHAT,IIODEV,_RAW) "\n"); return -999; }\
    fscanf(f,"%u", &raw);\
    fclose(f);\
    f  = fopen(HSK_PATH(WHAT,IIODEV,_offset),"r");\
    if (f) { fscanf(f,"%u", &offset); fclose(f); }\
    f  = fopen(HSK_PATH(WHAT,IIODEV,_scale),"r");\
    if (f) { fscanf(f,"%f", &scale); fclose(f); } \
    return (raw-offset)/scale;\
}

GEN_HSK(temp0,0)
GEN_HSK(temp0,1)

// could probably cache scale/offset values...
static void update_hsk()
{
  hsk.core_temp_iio0 = hsk_get_temp0_iio0();
  hsk.core_temp_iio1 = hsk_get_temp0_iio1();
}



/** This thread adds additional metadata to events, then send them */ 
static void* tx_thread(void *p)
{
  int fd = *((int*) p);

  struct timespec last_hsk_measure = { 0};

  while (state < SNAPHOSE_DIE)
  {
    snaphose_data_t * d = 0;

    while ((d = buffer_retrieve()))
    {
      struct timespec now;
      struct timespec now_rt;
      clock_gettime(CLOCK_MONOTONIC, &now);
      clock_gettime(CLOCK_REALTIME, &now_rt);
      d->nfreqbins = nsamples;
      d->nbits_per_bin = data_bits;
      d->sys_board_id = 0; // TODO
      d->sys_rev = 0; // TODO
      d->hsk.uptime.snaphose = (now.tv_sec - program_start.tv_sec)/60;
      d->hsk.uptime.red_pitaya = (now.tv_sec)/60;
      d->send_cpu_time.utc_secs = now_rt.tv_sec;
      d->send_cpu_time.utc_nsecs = now_rt.tv_nsec;
      if ( now.tv_sec - last_hsk_measure.tv_sec > 5)
      {
        update_hsk();
        memcpy(&last_hsk_measure, &now, sizeof(now));
      }
      d->hsk.temps.red_pitaya = hsk.core_temp_iio1; // ?? I think thi sis the right one

      send(fd, d, SNAPHOSE_DATA_SIZE_NEEDED(data_bits, nsamples), 0);
      buffer_drop();
    }

    // update hsk every once in a while
    usleep(1000);
  }

  return NULL;
}

static void* ctrl_thread(void* p)
{
  int fd = *((int*) p);

  //wait for connections
  while (state < SNAPHOSE_DIE)
  {
    struct sockaddr_in peer_addr;
    unsigned peer_addr_sz = sizeof(peer_addr);
    errno = 0;
    int client = accept(fd, (struct sockaddr*) &peer_addr, &peer_addr_sz);
    if (client < 0)
    {
      if (errno == EINTR) continue;
      else fprintf(stderr,"Got %s in accept\n", strerror(errno));
      state = SNAPHOSE_FAIL;
      break;
    }

    printf("Got client connection from %s\n", inet_ntoa(peer_addr.sin_addr));

    //wait for messages
    while (state < SNAPHOSE_DIE)
    {

      struct pollfd pfd = { .fd = client, .events = POLLIN | POLLPRI};
      errno = 0;
      int r  = poll(&pfd, 1, 100);

      if (!r) continue;
      if (r < 0)
      {
        fprintf(stderr,"ctrl_thread got err %d (%s)\n", errno, strerror(errno));
        continue;
      }

      //we have something available, let's read it
      if (pfd.revents & POLLIN)
      {
        errno = 0;
        char cmd = 0;
        r = recv(client, &cmd, sizeof(cmd), 0);
        if (r < 0)
        {
          fprintf(stderr,"CTRL THREAD: Got err %d (%s) in recv\n", errno, strerror(errno));
          continue;
        }
        printf("CMD: %c\n", cmd);
        // avoid CSWTCH generation due to static analyzer bug
        volatile unsigned char ucmd = cmd;

        //analyzer false positive here, I think / hope
        switch(ucmd)
        {
          // something that starts a new run
          case SNAPHOSE_CTRL_NOISE:
          case SNAPHOSE_CTRL_ANT:
          case SNAPHOSE_CTRL_TERM:
            drain = true;  // always drain first when we got this
            source = cmd == SNAPHOSE_CTRL_NOISE ? SNAPHOSE_SRC_NOISE :
                     cmd == SNAPHOSE_CTRL_ANT   ? SNAPHOSE_SRC_ANT :
                     cmd == SNAPHOSE_CTRL_TERM  ? SNAPHOSE_SRC_TERM :
                     SNAPHOSE_SRC_UNKNOWN;
            state = SNAPHOSE_RUN;
            break;

          //pause
          case SNAPHOSE_CTRL_PAUSE:
            drain = true;
            state = SNAPHOSE_PAUSE;
            break;
          case SNAPHOSE_CTRL_DIE:
            state = SNAPHOSE_DIE;
            break;
          case ' ':
          case '\n':
          case '\r':
          case '\t':
            break; // ignore ws
          default:
            fprintf(stderr," Unrecognized cmd\n");

        }
      }
      if (pfd.revents & POLLPRI)
      {
        fprintf(stderr,"Unexpected priority event?");
      }


    }


    close(client);

  }

  return NULL;
}

static int snap_arm(size_t i)
{
  if (verbose) printf("snap %zu arming\n",i);
  return write_reg(setup[i].ctrl, setup[i].ctrl_flags & ~SNAP_CTRL_ENABLE)
  || write_reg(setup[i].ctrl, setup[i].ctrl_flags & ~SNAP_CTRL_ENABLE);
}


static void*  read_thread(void *)
{
  uint32_t read_counter = 0;

  while (state < SNAPHOSE_DIE)
  {

    while ( state < SNAPHOSE_RUN)
    {

       // not taking data , sleep for one ms
       usleep(1000);
    }

    size_t start_snap = 0;
    //grab an available buffer
    snaphose_data_t * d = buffer_acquire();

    while (state == SNAPHOSE_RUN)
    {
      if (drain)
      {

        for (size_t isnap = 0; isnap < nblocks; isnap++)
        {
          setup[isnap].captures_since_drain = 0;
          snap_arm(isnap);
        }

        // we have reset everything
        drain = false;
      }



      //now loop through blocks to see if anything is ready

      for (size_t unwrapped_isnap = start_snap; unwrapped_isnap < start_snap+nblocks ; unwrapped_isnap++)
      {
        size_t isnap = unwrapped_isnap % nblocks;

        uint32_t status = 0;
        read_reg(setup[isnap].status, &status);
        if (status & SNAP_STATUS_DONE)
        {

          struct timespec now_rt;
          clock_gettime(CLOCK_REALTIME, &now_rt);

          // read various registers ASAP

          read_reg(reg.pps_counter, &d->pps_count_at_read_time);
          read_reg(reg.clk_cnts, &d->last_pps_at_read_time);
          read_reg(reg.pps_duration, &d->last_pps_duration);


          if (verbose)
            printf("snap %zu ready\n",isnap);


          // fill in info
          d->snap_index = isnap;

          //this can wait slightly
          read_reg(setup[isnap].val, &d->snap_cycle_count);

          //read out actual values
          //memcpy might not work here, so read one word at a time? 

          volatile uint32_t * start_word = (volatile uint32_t*) &fpga[setup[isnap].bram];
          uint32_t *output  =(uint32_t*) &d->packed_samples[0];

          volatile uint32_t * stop_word = start_word + (data_bits>>5) * nsamples;

          for (volatile uint32_t * word = start_word; word <= stop_word; )
          {
            *output++ = *word++;
          }

          // start next loop on next block
          start_snap =( isnap + 1) % nblocks;


          //we can arm it now
          snap_arm(isnap);


          d->source.which = source;
          d->read_counter = read_counter++;
          d->readout_cputime.utc_secs = now_rt.tv_sec;
          d->readout_cputime.utc_nsecs = now_rt.tv_nsec;
          clock_gettime(CLOCK_REALTIME, &now_rt);
          uint32_t us =  1e6* ( now_rt.tv_sec-d->readout_cputime.utc_secs) + 1e-3 * ( now_rt.tv_nsec - d->readout_cputime.utc_nsecs);
          d->us_elapsed_while_reading = us > 65535 ? 65535 : us;

          //commit buffer and grab another
          buffer_commit();
          d = buffer_acquire();
        }
      }

      //sleep 200 us
      usleep(200);
    }
  }
  return NULL;
}


