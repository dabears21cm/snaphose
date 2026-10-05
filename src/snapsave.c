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



static const char * data_out = "/data/spec/";
static uint32_t spec_per_file = 100;

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
    sprintf(scratchbuf,"%s/%d-%02d-%02d", data_out, year, month, day);
    errno = 0;
    if( mkdir(scratchbuf, 0755) && errno != EEXIST)
    {
      fprintf(stderr,"Problem making %s. This will probably end poorly. \n", scratchbuf);
    }

  }

  //check if we need to make a new hour dir
  if (tm->tm_hour != hour)
  {
    hour = tm->tm_hour;
    sprintf(scratchbuf,"%s/%d-%02d-%02d/%02d", data_out, year, month, day, hour);

    errno = 0;
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
    mkdir(scratchbuf, 0755);
    errno = 0;
    if( mkdir(scratchbuf, 0755) && errno != EEXIST)
    {
      fprintf(stderr,"Problem making %s. This will probably end poorly. \n", scratchbuf);
    }
  }

  sprintf(current_filename,"%s/%d-%02d-%02d/%02d/%02d/%c.%02d.%06d.dabears", data_out, year, month, day, hour, min, src, tm->tm_sec, (int) (now.tv_nsec /1000));

  errno = 0;
  int fd =  open(current_filename, O_CREAT | O_RDWR, 0644);
  if (fd < 0)
  {
    fprintf(stderr,"Problem making %s (err %d, %s). Probably going to make you sad\n", current_filename, errno, strerror(errno));
  }
  else
  {
    sprintf(scratchbuf, "%s/%c.CURRENT.dabears.tmp", data_out, src);
    unlink(scratchbuf);
    symlink(current_filename, scratchbuf);
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
    if (!strcmp(name,"output_dir")) data_out = strdup(val);
    else if (!strcmp(name,"spectra_per_file")) spec_per_file = atoi(val);
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
      fprintf(stderr,"Usage: snapsave [-f config-file = /etc/snaphose.ini]\n");
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


  
  struct sigaction sa;
  sa.sa_handler = im_done_now_k_thx;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  while (!done_now)
  {

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

    errno = 0;
    ssize_t r = recvmsg(rx_fd, &msg, 0);
    if (r < 0 && errno != EINTR)
    {
      fprintf(stderr,"Got err %d (%s) in recvmsg\n", errno, strerror(errno));
      ret = 1;
      goto cleanup;
    }

    if (r > 0)
    {
      if (sz && sz!=r)
      {
        fprintf(stderr,"WTF did the size change?\n");
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
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        d.d.rcv_cpu_time.utc_secs = now.tv_sec;
        d.d.rcv_cpu_time.utc_nsecs = now.tv_nsec;
      }


      //TODO fill in hsk information


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
        close(fds[fd_index]);
        fds[fd_index] = -1;
        nwritten[fd_index] = 0;
      }

      if (fds[fd_index] < 0) 
        fds[fd_index] = create_output_fd(d.d.source.which, sz);


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



