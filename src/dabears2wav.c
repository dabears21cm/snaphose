#define _GNU_SOURCE
#include "snaphose.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>


#include <sndfile.h>
#include "sonify.h"



SF_INFO info = {
    .samplerate = 48000,
    .channels   = 1,
    .format     = SF_FORMAT_WAV | SF_FORMAT_PCM_16,   /* or SF_FORMAT_FLOAT */
};

static union
{
  snaphose_data_t d;
  uint8_t dbuf[65527];
} d;


int main(int nargs, char ** args)
{

  SNDFILE *wf = sf_open(args[1], SFM_WRITE, &info);
  if (!wf) { fprintf(stderr, "sf_open: %s\n", sf_strerror(NULL)); /* ... */ }
  sf_command(wf, SFC_SET_CLIPPING, NULL, SF_TRUE);      /* clip, don't wrap */

  sonify_params p = { SONIFY_DEFAULT_PARAMS, .fmax=6250 };

  sonifier *s = sonify_create(&p, NULL);            /* baseline may be NULL */

  float * spec = malloc(1024 * sizeof(float));
  float *buf = malloc(sonify_max_samples(s) * sizeof(float));


  for (int i = 2; i < nargs; i++)
  {

    FILE *  f = NULL;
    int  (*closefn) ( FILE *) = NULL;
    // see if we need to decompress

    if (!strcmp(".zst", args[i] + (strlen(args[i]) - 4)))
    {
      char * cmd = NULL;
      asprintf(&cmd, "zstd -dc %s", args[i]);
      f = popen(cmd,"r");
      closefn = pclose;
      free(cmd);

    }
    else
    {
      f =  fopen(args[i], "r");
      closefn = fclose;
    }


    if (!f) 
    {
      fprintf(stderr, "Could not open %s\n", args[i]);
      continue;
    }
    while (!feof(f))
    {

      if (1 != fread(d.dbuf, sizeof(snaphose_data_t), 1, f))
      {
        if (feof(f)) break;
        fprintf(stderr,"Problem reading header. We probably won't recover from this\n");
        continue;
      }

      if (d.d.ver_magic != SNAPHOSE_VER_MAGIC)
      {
        fprintf(stderr," Bad magic %hx\n", d.d.ver_magic);
      }


      const size_t data_size = SNAPHOSE_PACKED_DATA_SIZE(d.d.nbits_per_bin, d.d.nfreqbins);

      if (data_size != fread(d.dbuf + sizeof(snaphose_data_t), 1, data_size, f))
      {
        fprintf(stderr,"Problem reading data\n");
        continue;
      }

      
      snaphose_unpack_samples_f32(&d.d, 1024, spec);


      size_t n = sonify_push(s, spec, buf);
      sf_write_float(wf, buf, (sf_count_t)n);

    }

    closefn(f);
  }

  sf_close(wf);
  sonify_destroy(s);
  free(buf);
  return 0;
}







