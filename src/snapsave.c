#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <systemd/sd-daemon.h>
#include <sys/types.h>
#include <poll.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "snaphose.h"
#include <signal.h>
#include "./ini.h"

#if defined(__has_include) && __has_include(<systemd/sd-json.h>)
#include <systemd/sd-json.h>
#define HAVE_SD_JSON
#else
#pragma message ("compiling without sd-json support")
#endif



static const char * data_out = "/data/spec/";
static uint32_t spec_per_file = 100;
static const char *hsk_file = "/data/housekeeping.json";
static int hsk_interval = 10;

static short udp_port = 2100;
static const char * bindto = "0.0.0.0";

static bool verbose = false;
static int watchdog_interval = 10;
static int print_every = 0;

static volatile int done_now;
static int done_sig;

static void im_done_now_k_thx(int sig)
{
  done_sig = sig;
  done_now = 1;
}

static char current_filename[1024];

int create_output_fd(char src, size_t sz)
{
  static char scratchbuf[1024];
  static int year = -1;
  static int month = -1;
  static int day = -1;
  static int hour = -1;
  static int min = -1;


  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);

  struct tm * tm = gmtime(&now.tv_sec);

  //chcek if we need new y-m-d
  if ( (tm->tm_year + 1900 != year) ||  ( tm->tm_mon +1 != month) || (tm->tm_mday != day))
  {
    year = tm->tm_year + 1900;
    month = tm->tm_mon + 1;
    day = tm->tm_mday;
    min = -1;
    hour = -1;
    sprintf(scratchbuf,"%s/%d-%02d-%02d", data_out, year, month, day);
    if( mkdir(scratchbuf, 0755) && errno != EEXIST)
    {
      fprintf(stderr,"Problem making %s. This will probably end poorly. \n", scratchbuf);
    }

  }

  //check if we need to make a new hour dir
  if (tm->tm_hour != hour)
  {
    hour = tm->tm_hour;
    min = -1;
    sprintf(scratchbuf,"%s/%d-%02d-%02d/%02d", data_out, year, month, day, hour);

    if( mkdir(scratchbuf, 0755) && errno != EEXIST)
    {
      fprintf(stderr,"Problem making %s. This will probably end poorly. \n", scratchbuf);
    }


  }

  //check if we need to make a new minute dir
  if (tm->tm_min != min)
  {
    min = tm->tm_min;
    sprintf(scratchbuf,"%s/%d-%02d-%02d/%02d/%02d", data_out, year, month, day, hour, min);
    if( mkdir(scratchbuf, 0755) && errno != EEXIST)
    {
      fprintf(stderr,"Problem making %s. This will probably end poorly. \n", scratchbuf);
    }
  }

  sprintf(current_filename,"%s/%d-%02d-%02d/%02d/%02d/%c.%02d.%06d.dabears", data_out, year, month, day, hour, min, src, tm->tm_sec, (int) (now.tv_nsec /1000));

  int fd =  open(current_filename, O_CREAT | O_RDWR, 0644);
  if (fd < 0)
  {
    fprintf(stderr,"Problem making %s (err %d, %s). Probably going to make you sad\n", current_filename, errno, strerror(errno));
    return fd;
  }
  else
  {
    sprintf(scratchbuf, "%s/%c.CURRENT.dabears.tmp", data_out, src);
    unlink(scratchbuf);
    if (symlink(current_filename, scratchbuf))
    {
      fprintf(stderr, "symlink failed\n");
    }
  }

  if (fallocate(fd, 0, 0, sz * spec_per_file))
  {
    fprintf(stderr, "fallocate failed\n");
  }

  return fd;
}







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

  if (!strcmp(section,"general"))
  {
    if (!strcmp(name,"verbose")) return !boolish(val, &verbose);
    else if (!strcmp(name,"watchdog")) watchdog_interval = atoi(val);
    else if (!strcmp(name,"print_every")) print_every = atoi(val);
    else
    {
      fprintf(stderr, "Unknown key %s.%s\n", section,name);
      return 0;
    }
  }

  else if (!strcmp(section,"net"))
  {
    if (!strcmp(name,"udp_port"))
    {
      int maybe_port = atoi(val);
      if (maybe_port > 0&& maybe_port < 65536)
        udp_port = maybe_port;
      else
        fprintf(stderr," Warning: invalid udp port specified, using default.\n");
    }
    else if (!strcmp(name,"bind_to"))
    {
      bindto = strdup(val);
    }
    else
    {
      fprintf(stderr, "Unknown key %s in %s\n", name, section);
      return 0;
    }
  }

  else if (strstr(section,"data"))
  {
    if (!strcmp(name,"output_dir"))
    {
      data_out = strdup(val);
      if (!data_out || strlen(data_out) > 64)
      {
        fprintf(stderr,"Unreasonably long data output directory.\n");
        return 0;
      }
    }
    else if (!strcmp(name,"spectra_per_file")) spec_per_file = atoi(val);
    else if (!strcmp(name,"hsk_file")) hsk_file = strdup(val);
    else if (!strcmp(name,"hsk_interval")) hsk_interval = atoi(val);
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

#define HSK_FIELDS(X_DBL,X_BOOL, X_INT)  \
 X_DBL(pi_cpu_c)    \
 X_DBL(pi_fan_rpm)  \
 X_DBL(pi_5v_v)     \
 X_DBL(probe_c) \
 X_DBL(cal_board_c) \
 X_DBL(drive_c)     \
 X_DBL(disk_free_gb)     \
 X_DBL(disk_free_pct)     \
 X_INT(throttled)   \
 X_DBL(load_1m)     \
 X_DBL(mem_avail_mb)     \
 X_DBL(uptime_h)     \
 X_INT(atten_A) \
 X_INT(atten_N) \
 X_INT(atten_T) \

#define X_DBL_DEF(X) double X;
#define X_BOOL_DEF(X) int X;
#define X_INT_DEF(X) int X;

static struct pi_hsk
{
  HSK_FIELDS(X_DBL_DEF,X_BOOL_DEF, X_INT_DEF)
}hsk;


static void update_hsk()
{
#ifndef HAVE_SD_JSON
  fprintf(stderr,"Trying to update_hsk but don't have sd-json\n");
#else
  sd_json_variant * json = 0;
  uint32_t retline = 0;
  uint32_t retcol = 0;
  int r = sd_json_parse_file(NULL, hsk_file,0, &json, &retline, &retcol);
  if (r)
  {
    fprintf(stderr,"Could not parse %s (%u:%u)\n", hsk_file, retline, retcol);
    return;
  }

#define X_DBL_DISPATCH(X) {#X , SD_JSON_VARIANT_NUMBER, sd_json_dispatch_double, offsetof(struct pi_hsk, X)},
#define X_INT_DISPATCH(X) {#X , SD_JSON_VARIANT_NUMBER, sd_json_dispatch_int32, offsetof(struct pi_hsk, X)},
#define X_BOOL_DISPATCH(X) {#X , SD_JSON_VARIANT_BOOLEAN, sd_json_dispatch_stdbool, offsetof(struct pi_hsk, X)},

  static const sd_json_dispatch_field table [] = {
  HSK_FIELDS(X_DBL_DISPATCH, X_BOOL_DISPATCH, X_INT_DISPATCH)
  {0}
  };

  r = sd_json_dispatch(json, table, SD_JSON_ALLOW_EXTENSIONS, &hsk);
  sd_json_variant_unref(json);
  if (r < 0)
  {
    fprintf(stderr,"hsk dispatching failed\n");
  }
#endif
}


int main(int nargs, char ** args)
{

  int fds[3] = {-1,-1,-1};
  size_t sz = 0;
  uint32_t nwritten[3] = {0};


  struct timespec program_start;
  clock_gettime(CLOCK_MONOTONIC,&program_start);
  const char  * setup_file = "/etc/snapsave.ini";
  //parse arguments
  for (int iarg = 1; iarg < nargs; iarg++)
  {
    if (!strcmp(args[iarg],"-f")  && iarg < nargs-1)
    {
      setup_file = args[++iarg];
    }
    else
    {
      fprintf(stderr,"Usage: snapsave [-f config-file = /etc/snapsave.ini]\n");
      return 1;
    }
  }

  //parse setup file
  if (read_setup_file(setup_file))
  {
    return 1;
  }


  int ret = 0;
  int rx_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (rx_fd <= 0)
  {
    fprintf(stderr, "Could not open UDP socket\n");
    ret = 1;
    goto cleanup;
  }

  //allow reuse
  int opt = 1;
  if (setsockopt(rx_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
  {
    fprintf(stderr,"SO_REUSEADDR FAIL\n");
    ret = 1;
    goto cleanup;
  }

  //bigger buf

  int rcvbuf_size = 1024 * 1024 *32;  // like, at least 2 seconds of data can be buffered.

  if (setsockopt(rx_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_size, sizeof(rcvbuf_size)))
  {
    fprintf(stderr,"SO_RCVBUF FAIL, do you need to make rmem_max bigger?\n");
    ret = 1;
    goto cleanup;
  }

  if (setsockopt(rx_fd, SOL_SOCKET, SO_TIMESTAMPNS, &opt, sizeof(opt)))
  {
    fprintf(stderr,"SO_TIMESTAMPNS FAIL, \n");
    ret = 1;
    goto cleanup;
  }

  struct sockaddr_in listen_addr= {
                                    .sin_family = AF_INET,
                                    .sin_addr = { .s_addr = inet_addr(bindto)},
                                    .sin_port = htons(udp_port)
  };

  if (bind(rx_fd, (struct sockaddr*) &listen_addr, sizeof(listen_addr)) < 0)
  {
    fprintf(stderr,"Failed to bind\n");
    ret = 1;
    goto cleanup;
  }


  struct timespec last_watchdog = {0};
  struct timespec last_hsk = {0};
  struct sigaction sa;
  sa.sa_handler = im_done_now_k_thx;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  sd_notify(0,"READY=1");

  uint32_t last_clk = 0;

  while (!done_now)
  {

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
    if (now.tv_sec - last_watchdog.tv_sec > watchdog_interval)
    {
      sd_notify(0, "WATCHDOG=1");
      memcpy(&last_watchdog, &now, sizeof(now));
    }

    if (now.tv_sec - last_hsk.tv_sec > hsk_interval)
    {
      update_hsk();
      memcpy(&last_hsk, &now, sizeof(now));
    }

    static union
    {
      snaphose_data_t d ;
      uint8_t dbuf[65527];  // can hold any UDP packet
    } d;

    struct iovec msg_iov = { &d, sizeof(d) };

    static char ctrl_buf[128];

    struct msghdr msg = {
      .msg_iov = &msg_iov,
      .msg_iovlen = 1,
      .msg_control = ctrl_buf,
      .msg_controllen = sizeof(ctrl_buf),
    };

    //poll the rx_fd. Can't wait forever becaues then watchdog would kill us
    struct pollfd pfd =  {.fd = rx_fd, .events = POLLIN };

    int pollret = poll (&pfd,1, 1000);

    if (pollret == 0) continue;

    if (pollret  < 0)
    {
      if (errno != EINTR)
      {
        fprintf(stderr,"Error %d in poll\n", errno);
        break;
      }
      continue;
    }

    if (0 == (pfd.revents & POLLIN)) continue;

    ssize_t r = recvmsg(rx_fd, &msg, 0);

    if (r < 0 && errno != EINTR)
    {
      fprintf(stderr,"Got err %d (%s) in recvmsg\n", errno, strerror(errno));
      ret = 1;
      goto cleanup;
    }

    if (r > 0)
    {
      if (r < sizeof(snaphose_data_t) || d.d.ver_magic != SNAPHOSE_VER_MAGIC)
      {
        fprintf(stderr,"Malformed snaphose data?\n");
        continue;
      }
      if (sz && sz!=r)
      {
        fprintf(stderr,"WTF did the size change? Gonna clean up and get resurrected by systemd.\n");
        ret = 1;
        goto cleanup;
      }

      sz = r;

      struct cmsghdr * cmsg = CMSG_FIRSTHDR(&msg);
      //timestamp from kernel
      if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPNS)
      {
        struct timespec * rcv_time = (struct timespec *) CMSG_DATA(cmsg);
        d.d.rcv_cpu_time.utc_secs = rcv_time->tv_sec;
        d.d.rcv_cpu_time.utc_nsecs = rcv_time->tv_nsec;
      }
      else //we gotta provide it
      {
        struct timespec now_rt;
        clock_gettime(CLOCK_REALTIME, &now_rt);
        d.d.rcv_cpu_time.utc_secs = now_rt.tv_sec;
        d.d.rcv_cpu_time.utc_nsecs = now_rt.tv_nsec;
      }
      d.d.source.attenuation = d.d.source.which == 'A' ? hsk.atten_A :
                               d.d.source.which == 'N' ? hsk.atten_N :
                               d.d.source.which == 'T' ? hsk.atten_T :
                               0;


      //TODO fill in hsk information

      d.d.hsk.uptime.snapsave = (now.tv_sec - program_start.tv_sec)/60;
      d.d.hsk.uptime.rpi = (now.tv_sec)/60;
      d.d.hsk.rpi.disk_free_GiB = hsk.disk_free_gb;
      d.d.hsk.rpi.fan_rpm = hsk.pi_fan_rpm;
      d.d.hsk.rpi.rail5V_mV = hsk.pi_5v_v*1000;
      d.d.hsk.rpi.throttled = hsk.throttled;
      d.d.hsk.rpi.free_mem_MB = hsk.mem_avail_mb;
      d.d.hsk.rpi.core_temp = hsk.pi_cpu_c;
      d.d.hsk.temps.w1_probe = hsk.probe_c;
      d.d.hsk.temps.cal_board = hsk.cal_board_c;
      d.d.hsk.temps.ssd = hsk.drive_c;

      if (verbose)
      {
        printf("Got packet, delta_clocks = %u\n", d.d.snap_cycle_count - last_clk);
        last_clk = d.d.snap_cycle_count;
      }

      //get file descriptor
      int fd_index = d.d.source.which == 'A' ? 0  :
                     d.d.source.which == 'N' ? 1 :
                     d.d.source.which == 'T' ? 2 :
                     -1;


      if (fd_index < 0)
      {
        fprintf(stderr,"Unexpected source %c, skipping\n", d.d.source.which);
        continue;
      }

      if (nwritten[fd_index] >= spec_per_file)
      {
        printf("..done\n");
        close(fds[fd_index]);
        fds[fd_index] = -1;
        nwritten[fd_index] = 0;
        static char buf1[128];
        //TODO can reuse these buffers rather than recreating them each time lol
        sprintf(buf1, "%s/%c.CURRENT.dabears.tmp", data_out, d.d.source.which);
        static char buf2[128];
        sprintf(buf2, "%s/%c.CURRENT.dabears", data_out, d.d.source.which);
        if (rename(buf1,buf2))
        {
          fprintf(stderr, "symlink rename problem\n");
        }
      }

      if (fds[fd_index] < 0) 
      {
        fds[fd_index] = create_output_fd(d.d.source.which, sz);
        printf("Created %s...",current_filename);
        fflush(stdout);
      }


      size_t nwr = 0;

      while (nwr < sz)
      {
        errno = 0;
        int w = write(fds[fd_index], ((char*) &d) + nwr, sz - nwr);

        if (w < 0  && errno != EINTR)
        {
          fprintf(stderr," Got err %d (%s) while writing\n", errno, strerror(errno));
          ret = 1;
          goto cleanup;
        }
        else if (w > 0) nwr += w;
      }
      nwritten[fd_index]++;
    }
  }




cleanup:

  if (done_sig) printf("signal %d put me out of my misery\n", done_sig);
  close(rx_fd);

  for (int i = 0 ; i < 3; i++)
  {
    if (fds[i] > 0)
    {
      if (ftruncate(fds[i], nwritten[i]  * sz))
      {
        fprintf(stderr,"Couldn't truncate\n");
      }
      close(fds[i]);
    }
  }

  return ret;

}



