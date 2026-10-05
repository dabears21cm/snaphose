#include "snaphose.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

static union
{
  snaphose_data_t d;
  uint8_t dbuf[65527];
} d;




int main(int nargs, char ** args)
{


  for (int i = 1; i < nargs; i++)
  {
    FILE * f =  fopen(args[i], "r");

    if (!f) 
    {
      fprintf(stderr, "Could not open %s\n", args[i]);
      continue;
    }
    while (!feof(f))
    {

      if (1 != fread(d.dbuf, sizeof(snaphose_data_t), 1, f))
      {
        fprintf(stderr,"Problem reading header. We probably won't recover from this\n");
        continue;
      }

      if (d.d.ver_magic != 0x5150)
      {
        fprintf(stderr," Bad magic %hx\n", d.d.ver_magic);
      }


      const size_t data_size = SNAPHOSE_PACKED_DATA_SIZE(d.d.nbits_per_bin, d.d.nfreqbins);

      if (data_size != fread(d.dbuf + sizeof(snaphose_data_t), 1, data_size, f))
      {
        fprintf(stderr,"Problem reading data\n");
        continue;
      }

      snaphose_dump(stdout, &d.d);
    }

    fclose(f);
  }


  return 0;
}


