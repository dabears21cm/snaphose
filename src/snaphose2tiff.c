#include "snaphose.h"
#include "tiffio.h"
#include <string.h>
#include <stdlib.h>


static union
{
  snaphose_data_t d;
  uint8_t dbuf[65527];
} d;


int main(int nargs, char ** args)
{

  if (nargs < 2 || !strstr(args[1],".tif"))
  {
    fprintf(stderr,"Usage:  snaphose2tiff out.tiff in.dabears [in2.dabears ... ]\n");
  }

  TIFF * tif = TIFFOpen(args[1],"w");


  float * frow = NULL;



  uint32_t y = 0;
  for (int i = 2; i < nargs; i++)
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


      if (!frow)
      {

        TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, d.d.nfreqbins);
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL,1);
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE,32);
        TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ZSTD);
        TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_FLOATINGPOINT);
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif,0));
        frow = malloc ( d.d.nfreqbins * sizeof(float));
      }

      snaphose_unpack_samples_f32( &d.d, d.d.nfreqbins, frow);
      TIFFWriteScanline(tif, frow, y++, 0);
    }

    fclose(f);
  }

  free(frow);
  TIFFClose(tif);


  return 0;
}


