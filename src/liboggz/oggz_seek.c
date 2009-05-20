/*
   Copyright (C) 2009 Annodex Association

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   - Neither the name of the Annodex Association nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
   PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE ASSOCIATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 * oggz_seek.c
 *
 * Refer to http://wiki.xiph.org/index.php/Seeking for an overview of the
 * algorithm for seeking on Ogg.
 *
 * Define seek to mean: for each logical bitstream, locate the
 * bytewise-latest page in the bitstream with a time < the target
 * time, then choose the bytewise-earliest among these pages. Thus
 * if two pages have the same time, seeking will locate the
 * bytewise-earlier page. 
 *
 * Conrad Parker <conrad@annodex.net>
 */

#include "config.h"

#if OGGZ_CONFIG_READ

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <errno.h>

#include <ogg/ogg.h>

#include "oggz_compat.h"
#include "oggz_private.h"
#include "oggz/oggz_packet.h"
#include "oggz/oggz_table.h"

/* #define DEBUG */
/* #define DEBUG_VERBOSE */

/************************************************************
 * OggzSeekInfo
 */

typedef struct _OggzSeekInfo OggzSeekInfo;

struct _OggzSeekTrack {
  /* Offset of bytewise-latest page with time < target */
  oggz_off_t latest_page_offset;
};

struct OggzSeekCache {
  time_t mtime;
  off_t size;
  off_t last_page_offset;
  long unit_end;
};

struct _OggzSeekInfo {
  OGGZ * oggz;

  struct OggzSeekCache cache;

  long unit_target;

  off_t offset_at;
  long unit_at;
  ogg_page og_at;

  size_t current_page_bytes;

  OggzTable * tracks;
};

/************************************************************
 * Primitives
 */

#define PAGESIZE 4096

/*
 * Raw seek
 */

static int
oggz_seek_reset_stream(void *data) {
  oggz_stream_t * stream = (oggz_stream_t *) data;

  ogg_stream_reset (&stream->ogg_stream);
  stream->last_granulepos = -1L;

  return 0;
}

static int
oggz_reset (OGGZ * oggz)
{
  OggzReader * reader = &oggz->x.reader;
  oggz_off_t offset_at;

  offset_at = oggz->offset = oggz_io_tell (oggz);

  ogg_sync_reset (&reader->ogg_sync);

  oggz_vector_foreach(oggz->streams, oggz_seek_reset_stream);
  
  /* Reset page reader state */
  reader->current_serialno = -1;
  reader->current_page_bytes = 0;

  reader->current_packet_pages = 0;
  reader->current_packet_begin_page_offset = oggz->offset;
  reader->current_packet_begin_segment_index = 1;

  return offset_at;
}

/* XXX: Should this take a seek_info? need to keep seek_info->offset_at in sync
 * with oggz->offset, on API entry (before work) and on return.
 */
static oggz_off_t
oggz_seek_raw (OGGZ * oggz, oggz_off_t offset, int whence)
{
  if (oggz_io_seek (oggz, offset, whence) == -1) {
#ifdef DEBUG
    printf ("%s: oggz_io_seek() returned -1\n", __func__);
#endif
    return -1;
  }

  return oggz_reset (oggz);
}

/*
 * Scan forwards to the next Ogg page boundary, >= the current
 * position, and load that page.
 * \returns Offset of next page
 * \retval -1 Error
 * \retval -2 EOF
 */
static off_t
page_next (OggzSeekInfo * seek_info)
{
  OGGZ * oggz = seek_info->oggz;
  ogg_page * og = &seek_info->og_at;
  OggzReader * reader;
  long serialno;
  ogg_int64_t granulepos;
  char * buffer;
  long bytes = 0, more;
  oggz_off_t page_offset = 0, ret;
  int found = 0;

  reader = &oggz->x.reader;

  seek_info->offset_at += seek_info->current_page_bytes;
  seek_info->current_page_bytes = 0;

  do {
    more = ogg_sync_pageseek (&reader->ogg_sync, og);

    if (more == 0) {
      page_offset = 0;

      buffer = ogg_sync_buffer (&reader->ogg_sync, PAGESIZE);
      if ((bytes = (long) oggz_io_read (oggz, buffer, PAGESIZE)) == 0) {
	if (oggz->file && feof (oggz->file)) {
#ifdef DEBUG_VERBOSE
	  printf ("%s: feof (oggz->file), returning -2\n", __func__);
#endif
	  clearerr (oggz->file);
	  ret = -2;
          goto page_next_fail;
	}
      }
      if (bytes == OGGZ_ERR_SYSTEM) {
	  /*oggz_set_error (oggz, OGGZ_ERR_SYSTEM);*/
	  ret = -1;
          goto page_next_fail;
      }

      if (bytes == 0) {
#ifdef DEBUG_VERBOSE
	printf ("%s: bytes == 0, returning -2\n", __func__);
#endif
	ret = -2;
        goto page_next_fail;
      }

      ogg_sync_wrote(&reader->ogg_sync, bytes);

    } else if (more < 0) {
#ifdef DEBUG_VERBOSE
      printf ("%s: skipped %ld bytes\n", __func__, -more);
#endif
      //page_offset -= more;
      seek_info->offset_at += (-more);
    } else {
#ifdef DEBUG_VERBOSE
      printf ("%s: page has %ld bytes\n", __func__, more);
#endif
      seek_info->current_page_bytes = more;
      found = 1;
    }

  } while (!found);

#if 0
  /* Calculate the byte offset of the page which was found */
  if (bytes > 0) {
    seek_info->offset_at = oggz_io_tell (oggz) - bytes + page_offset;
  } else {
    /* didn't need to do any reading -- accumulate the page_offset */
    seek_info->offset_at = seek_info->offset_at + page_offset;
  }
  
  ret = seek_info->offset_at + more;
#else
  ret = seek_info->offset_at;
#endif

page_next_ok:

  serialno = ogg_page_serialno (og);
  granulepos = ogg_page_granulepos (og);
  seek_info->unit_at = oggz_get_unit (oggz, serialno, granulepos);

#ifdef DEBUG
  printf ("%s: serialno %010ld, granulepos %lld\tunit %ld\n", __func__,
          serialno, granulepos, seek_info->unit_at);
#endif

  reader->current_unit = seek_info->unit_at;

  return ret;

page_next_fail:

  /* XXX: reset to oggz->offset etc. */
  oggz_io_seek (oggz, oggz->offset, SEEK_SET);

  return ret;
}

/*
 * Return values as for page_next()
 */
static off_t
page_at_or_after (OggzSeekInfo * seek_info, oggz_off_t offset)
{
  OGGZ * oggz = seek_info->oggz;

  seek_info->offset_at = oggz_seek_raw (oggz, offset, SEEK_SET);

  return page_next (seek_info);
}

/************************************************************
 * Cache update
 */

/*
 * Find the last page which has a granulepos.
 * Update its offset and unit in our cache.
 */
static int
update_last_page (OggzSeekInfo * seek_info)
{
  OGGZ * oggz = seek_info->oggz;
  OggzReader * reader;

  reader = &oggz->x.reader;

  if (oggz_io_seek (seek_info->oggz, -4096, SEEK_END) == -1) {
    printf ("%s: oggz_io_seek() returned -1\n", __func__);
    return -1;
  }

  ogg_sync_reset (&reader->ogg_sync);

  while (page_next (seek_info) >= 0) {
          seek_info->cache.unit_end = reader->current_unit;
  }

  return 0;
}

static int
update_seek_cache (OggzSeekInfo * seek_info)
{
  struct stat statbuf;
  int fd;
  oggz_off_t offset_save;
  OGGZ * oggz = seek_info->oggz;

  seek_info->offset_at = oggz->offset;

  if (oggz->file == NULL) {
    /* Can't check validity, just update end */
    offset_save = oggz_io_tell (oggz);
    if (oggz_io_seek (oggz, 0, SEEK_END) == -1) {
      return -1;
    }

    seek_info->cache.size = oggz_io_tell (oggz);

    if (oggz_io_seek (oggz, offset_save, SEEK_SET) == -1) {
      return -1; /* fubar */
    }
  } else {
    if ((fd = fileno (oggz->file)) == -1) {
      return -1;
    }

    if (fstat (fd, &statbuf) == -1) {
      switch (errno) {
      default:
        return -1;
      }
    }

    if (oggz_stat_regular (statbuf.st_mode)) {
      if (seek_info->cache.mtime == statbuf.st_mtime) {
        /* Not modified, cache is valid */
        return 0;
      }
    }

    seek_info->cache.mtime = statbuf.st_mtime;
    seek_info->cache.size = statbuf.st_size;
  }

  update_last_page (seek_info);

  return 1;
}

/************************************************************
 * Public API
 */

/*
 * The typical usage is:
 *
 *   oggz_set_data_start (oggz, oggz_tell (oggz));
 */
int
oggz_set_data_start (OGGZ * oggz, oggz_off_t offset)
{
  if (oggz == NULL) return -1;

  if (offset < 0) return -1;

  oggz->offset_data_begin = offset;

  return 0;
}

int
oggz_purge (OGGZ * oggz)
{
  return -1;
}

off_t
oggz_seek (OGGZ * oggz, oggz_off_t offset, int whence)
{
  OggzSeekInfo seek_info;
  OggzReader * reader;
  off_t result;
  ogg_page * og;

  memset (&seek_info, 0, sizeof(OggzSeekInfo));

  seek_info.oggz = oggz;

  update_seek_cache (&seek_info);

  switch (whence) {
  case SEEK_CUR:
    offset += oggz->offset;
    break;
  case SEEK_END:
    offset = seek_info.cache.size - offset;
    break;
  case SEEK_SET:
  default:
    break;
  }

  result = page_at_or_after (&seek_info, offset);

  reader = &oggz->x.reader;
  reader->current_page_bytes = 0;

  og = &seek_info.og_at;
  reader->expect_hole = ogg_page_continued(og);

  return oggz_seek_raw (oggz, result, SEEK_SET);
}

long
oggz_seek_units (OGGZ * oggz, ogg_int64_t units, int whence)
{
  return -1;
}

off_t
oggz_seek_position (OGGZ * oggz, oggz_position * position)
{
  OggzReader * reader;

  if (oggz == NULL)
    return OGGZ_ERR_BAD_OGGZ;

  if (oggz->flags & OGGZ_WRITE)
    return OGGZ_ERR_INVALID;

  if (oggz_seek_raw (oggz, position->begin_page_offset, SEEK_SET) == -1) {
#ifdef DEBUG
     printf ("%s: oggz_seek_raw() returned -1\n", __func__);
#endif
     return -1;
   }

  oggz->offset = position->begin_page_offset;

  reader = &oggz->x.reader;
  reader->current_unit = -1;

  /* Set up the position info */
  reader->current_granulepos = position->calc_granulepos;
  reader->current_packet_pages = position->pages;
  reader->current_packet_begin_page_offset = position->begin_page_offset;
  reader->current_packet_begin_segment_index = position->begin_segment_index;

  /* Tell oggz_read_sync() that the position info is set up, so it can
   * simply skip over packets until the requested segment is found, then
   * deliver as normal.
   * The actual data fetching is ensured by the next invocation of
   * oggz_read*(). */
  reader->position_ready = 1;

  return oggz->offset;
}

long
oggz_get_duration (OGGZ * oggz)
{
  OggzSeekInfo seek_info;

  memset (&seek_info, 0, sizeof(OggzSeekInfo));

  seek_info.oggz = oggz;

  update_seek_cache (&seek_info);

  return seek_info.cache.unit_end;
}

long
oggz_seek_byorder (OGGZ * oggz, void * target)
{
  return -1;
}

long
oggz_seek_packets (OGGZ * oggz, long serialno, long packets, int whence)
{
  return -1;
}

#else

#include <ogg/ogg.h>
#include "oggz_private.h"

off_t
oggz_seek (OGGZ * oggz, oggz_off_t offset, int whence)
{
  return OGGZ_ERR_DISABLED;
}

long
oggz_seek_units (OGGZ * oggz, ogg_int64_t units, int whence)
{
  return OGGZ_ERR_DISABLED;
}

long
oggz_seek_byorder (OGGZ * oggz, void * target)
{
  return OGGZ_ERR_DISABLED;
}

long
oggz_seek_packets (OGGZ * oggz, long serialno, long packets, int whence)
{
  return OGGZ_ERR_DISABLED;
}

#endif
