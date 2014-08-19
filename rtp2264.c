/*
 * Report on a pcap (.pcap) file.
 *
 * <rrw@kynesim.co.uk> 2008-09-05
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the MPEG TS, PS and ES tools.
 *
 * The Initial Developer of the Original Code is Amino Communications Ltd.
 * Portions created by the Initial Developer are Copyright (C) 2008
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Richard Watts, Kynesim <rrw@kynesim.co.uk>
 *
 * ***** END LICENSE BLOCK *****
 */

// H.264 over RTP is defined in RFC3984

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#ifdef _WIN32
#include <stddef.h>
#endif // _WIN32

#include "compat.h"
#include "version.h"
#include "misc_fns.h"
#include "fmtx.h"

#define RTP_HDR_LEN 8

#define RTP_PREFIX_STRING "RTP "
#define RTP_PREFIX_LEN    4
#define RTP_LEN_OFFSET    4

static int c642b(const char c)
{
  return (c >= 'A' && c <= 'Z') ? c - 'A' :
    (c >= 'a' && c <= 'z') ? c - 'a' + 26 :
    (c >= '0' && c <= '9') ? c - '0' + 52 :
    (c == '+' || c == '-') ? 62 :
    (c == '/' || c == '_') ? 63 :
    (c == '=') ? -1 : -2;
}

static size_t b64str2binn(byte * const dest0, const size_t dlen, const char ** const plast, const char * src)
{
  byte * dest = dest0;
  uint32_t a = 0;
  ssize_t i = 4;
  size_t slen = (dlen * 4 + 5) / 3;
  int b;

  while ((b = c642b(*src++)) >= 0 && --slen != 0)
  {
    a = (a << 6) | b;
    if (--i == 0)
    {
      *dest++ = (a >> 16) & 0xff;
      *dest++ = (a >> 8) & 0xff;
      *dest++ = a & 0xff;
      i = 4;
    }
  }

  // Tidy up at the end
  if (i < 3)  // i == 4 good, all done, i == 3 error
  {
    a <<= i * 6;
    *dest++ = (a >> 16) & 0xff;

    // Consume '='
    if (b == -1)
      b = c642b(*src++);

    if (i == 1)
    {
      *dest++ = (a >> 8) & 0xff;
    }
    else if (b == -1)
      ++src;
  }

  if (plast != NULL)
    *plast = src - 1;

  return dest - dest0;
}


int main(int argc, char **argv)
{
  FILE *f_in = NULL;
  FILE *f_out = NULL;
  const char * fname_in;
  const char * fname_out;
  int zcount = 0;

  if (argc < 3)
  {
    fprintf(stderr, "Usage: <in.rtp> <out.264>\n");
    return 1;
  }

  fname_in = argv[1];
  fname_out = argv[2];

  if ((f_in = fopen(fname_in, "rb")) == NULL)
  {
    perror(argv[1]);
    return 1;
  }

  if ((f_out = fopen(fname_out, "wb")) == NULL)
  {
    perror(argv[2]);
    return 1;
  }

  if (argc > 3)
  {
    byte psbuf[0x1000];
    const char * eo64 = argv[3];

    psbuf[0] = 0;
    psbuf[1] = 0;
    psbuf[2] = 0;
    psbuf[3] = 1;

    do
    {
      size_t len = b64str2binn(psbuf + 4, sizeof(psbuf) - 4, &eo64, eo64);

      if ((*eo64 != 0 && *eo64 != ',') || len == 0)
      {
        fprintf(stderr, "Bad B64 string: '%s' (len=%zd, chr=%d)\n", argv[3], len, *eo64);
        exit(1);
      }

      if (fwrite(psbuf, len + 4, 1, f_out) != 1)
      {
        perror(fname_out);
        exit(1);
      }
    } while (*eo64++ == ',');
  }

  for (;;)
  {
    byte buf[0x10000];
    uint32_t rtplen;

    if (fread(buf, RTP_HDR_LEN, 1, f_in) != 1)
    {
      if (ferror(f_in))
        perror(fname_in);
      break;
    }
    if (memcmp(buf, RTP_PREFIX_STRING, RTP_PREFIX_LEN) != 0)
    {
      fprintf(stderr, "### Bad RTP prefix\n");
      break;
    }
    rtplen = uint_32_be(buf + RTP_LEN_OFFSET);
    if (rtplen > sizeof(buf) || rtplen < 12)
    {
      fprintf(stderr, "### Bad RTP len: %" PRIu32 "\n", rtplen);
      break;
    }

    if (fread(buf, rtplen, 1, f_in) != 1)
    {
      if (ferror(f_in))
        perror(fname_in);
      else
        fprintf(stderr, "### Unexpected EOF\n");
      break;
    }

    {
      size_t offset = 12 + (buf[0] & 0xf) * 4;
      size_t padlen = ((buf[0] & 0x20) != 0) ? buf[rtplen - 1] : 0;

      // Check for extension
      if ((buf[0] & 0x10) != 0)  // X bit
        offset += 4 + uint_16_be(buf + offset + 2);

      if (rtplen < offset + padlen + 1)
      {
        fprintf(stderr, "### Bad RTP offset + padding\n");
      }

      // OK - got payload

      {
        const byte * p = buf + offset;
        const byte * p_end = buf + rtplen - padlen;
        byte buf2[0x18000]; // Allow for max expansion
        byte * q = buf2;
        byte sc1 = *p++;

        if ((sc1 & 0x1f) == 28)
        {
          byte sc2 = *p++;
          if ((sc2 & 0x80) != 0)  // S bit
          {
            // Start of fragmented unit
            sc1 = (sc1 & 0xe0) | (sc2 & 0x1f);
            *q++ = 0;
            *q++ = 0;
            *q++ = 0;
            *q++ = 1;
            *q++ = sc1;
            zcount = 0;

            printf("Fragmented block with code: %x\n", sc1);
          }
        }
        else
        {
          // Normal start code
          *q++ = 0;
          *q++ = 0;
          *q++ = 0;
          *q++ = 1;
          *q++ = sc1;
          zcount = 0;
          printf("Start block with code: %x\n", sc1);
        }


        // Engage emulation protect
        while (p < p_end)
        {
          const byte b = *p++;

          if (zcount == 2 && b <= 3)
          {
            *q++ = 3;
            zcount = 0;
          }

          *q++ = b;
          zcount = (b == 0) ? zcount + 1 : 0;
        }

        if (fwrite(buf2, q - buf2, 1, f_out) != 1)
        {
          perror(fname_out);
          exit(1);
        }
      }
    }

  }

  fclose(f_out);
  fclose(f_in);
  return 0;
}

