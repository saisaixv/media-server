#include "mov-writer.h"
#include "mov-internal.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <time.h>

struct mov_writer_t
{
	struct mov_t mov;
	size_t mdat_size;
	uint64_t mdat_offset;
};

static size_t mov_write_moov(struct mov_t* mov)
{
	size_t size, i;
	uint64_t offset;

	size = 8 /* Box */;
	offset = mov_buffer_tell(&mov->io);
	mov_buffer_w32(&mov->io, 0); /* size */
	mov_buffer_write(&mov->io, "moov", 4);

	size += mov_write_mvhd(mov);
//	size += mov_write_iods(mov);
	for(i = 0; i < mov->track_count; i++)
	{
		mov->track = mov->tracks + i;
		if (mov->track->sample_count < 1)
			continue;
		size += mov_write_trak(mov);
	}

//  size += mov_write_udta(mov);
	mov_write_size(mov, offset, size); /* update size */
	return size;
}

void mov_write_size(const struct mov_t* mov, uint64_t offset, size_t size)
{
	uint64_t offset2;
	offset2 = mov_buffer_tell(&mov->io);
	mov_buffer_seek(&mov->io, offset);
	mov_buffer_w32(&mov->io, size);
	mov_buffer_seek(&mov->io, offset2);
}

static int mov_writer_init(struct mov_t* mov)
{
	mov->ftyp.major_brand = MOV_BRAND_ISOM;
	mov->ftyp.minor_version = 0x200;
	mov->ftyp.brands_count = 4;
	mov->ftyp.compatible_brands[0] = MOV_BRAND_ISOM;
	mov->ftyp.compatible_brands[1] = MOV_BRAND_ISO2;
	mov->ftyp.compatible_brands[2] = MOV_BRAND_AVC1;
	mov->ftyp.compatible_brands[3] = MOV_BRAND_MP41;
	mov->header = 0;
	return 0;
}

struct mov_writer_t* mov_writer_create(const struct mov_buffer_t* buffer, void* param, int flags)
{
	struct mov_t* mov;
	struct mov_writer_t* writer;
	writer = (struct mov_writer_t*)calloc(1, sizeof(struct mov_writer_t));
	if (NULL == writer)
		return NULL;

	mov = &writer->mov;
	mov->flags = flags;
	mov->io.param = param;
	memcpy(&mov->io.io, buffer, sizeof(mov->io.io));

	mov->mvhd.next_track_ID = 1;
	mov->mvhd.creation_time = time(NULL) + 0x7C25B080; // 1970 based -> 1904 based;
	mov->mvhd.modification_time = mov->mvhd.creation_time;
	mov->mvhd.timescale = 1000;
	mov->mvhd.duration = 0; // placeholder

	mov_writer_init(mov);
	mov_write_ftyp(mov);

	// mdat
	writer->mdat_offset = mov_buffer_tell(&mov->io);
	mov_buffer_w32(&mov->io, 0); /* size */
	mov_buffer_write(&mov->io, "mdat", 4);
	return writer;
}

static int mov_writer_move(struct mov_t* mov, uint64_t to, uint64_t from, size_t bytes);
void mov_writer_destroy(struct mov_writer_t* writer)
{
	size_t i;
	uint64_t offset, offset2;
	struct mov_t* mov;
	struct mov_track_t* track;
	mov = &writer->mov;

	// finish mdat box
	mov_write_size(mov, writer->mdat_offset, writer->mdat_size+8); /* update size */

	// finish sample info
	for (i = 0; i < mov->track_count; i++)
	{
		track = &mov->tracks[i];
		if(track->sample_count < 1)
			continue;

		// pts in ms
		track->mdhd.duration = track->samples[track->sample_count - 1].dts - track->samples[0].dts;
		//track->mdhd.duration = track->mdhd.duration * track->mdhd.timescale / 1000;
		track->tkhd.duration = track->mdhd.duration * mov->mvhd.timescale / track->mdhd.timescale;
		if (track->tkhd.duration > mov->mvhd.duration)
			mov->mvhd.duration = track->tkhd.duration; // maximum track duration
	}

	// write moov box
	offset = mov_buffer_tell(&mov->io);
	mov_write_moov(mov);
	offset2 = mov_buffer_tell(&mov->io);
	
	if (MOV_FLAG_FASTSTART & mov->flags)
	{
		// check stco -> co64
		uint64_t co64 = 0;
		for (i = 0; i < mov->track_count; i++)
		{
			co64 += mov_stco_size(&mov->tracks[i], offset2 - offset);
		}

		if (co64)
		{
			uint64_t sz;
			do
			{
				sz = co64;
				co64 = 0;
				for (i = 0; i < mov->track_count; i++)
				{
					co64 += mov_stco_size(&mov->tracks[i], offset2 - offset + sz);
				}
			} while (sz != co64);
		}

		// rewrite moov
		for (i = 0; i < mov->track_count; i++)
			mov->tracks[i].offset += (offset2 - offset) + co64;

		mov_buffer_seek(&mov->io, offset);
		mov_write_moov(mov);
		assert(mov_buffer_tell(&mov->io) == offset2 + co64);
		offset2 = mov_buffer_tell(&mov->io);

		mov_writer_move(mov, writer->mdat_offset, offset, (size_t)(offset2 - offset));
	}

	for (i = 0; i < mov->track_count; i++)
        mov_free_track(mov->tracks + i);
	free(writer);
}

static int mov_writer_move(struct mov_t* mov, uint64_t to, uint64_t from, size_t bytes)
{
	uint8_t* ptr;
	uint64_t i, j;
	void* buffer[2];

	assert(bytes < INT32_MAX);
	ptr = malloc((size_t)(bytes * 2));
	if (NULL == ptr)
		return -ENOMEM;
	buffer[0] = ptr;
	buffer[1] = ptr + bytes;

	mov_buffer_seek(&mov->io, from);
	mov_buffer_read(&mov->io, buffer[0], bytes);

	j = 1;
	mov_buffer_seek(&mov->io, to);
	for (i = to; i < from; i += bytes)
	{
		mov_buffer_seek(&mov->io, i);
		mov_buffer_read(&mov->io, buffer[j], bytes);

		j ^= 1;
		mov_buffer_seek(&mov->io, i);
		mov_buffer_write(&mov->io, buffer[j], bytes);
	}

	mov_buffer_write(&mov->io, buffer[j], bytes - (size_t)(i - from));
	free(ptr);
	return mov_buffer_error(&mov->io);
}

int mov_writer_write(struct mov_writer_t* writer, int track, const void* data, size_t bytes, int64_t pts, int64_t dts, int flags)
{
	struct mov_t* mov;
	struct mov_sample_t* sample;

	if (track < 0 || track >= (int)writer->mov.track_count)
		return -ENOENT;
	
	mov = &writer->mov;
	mov->track = &mov->tracks[track];

	if (mov->track->sample_count + 1 >= mov->track->sample_offset)
	{
		void* ptr = realloc(mov->track->samples, sizeof(struct mov_sample_t) * (mov->track->sample_offset + 1024));
		if (NULL == ptr) return -ENOMEM;
		mov->track->samples = ptr;
		mov->track->sample_offset += 1024;
	}

	pts = pts * mov->track->mdhd.timescale / 1000;
	dts = dts * mov->track->mdhd.timescale / 1000;

	sample = &mov->track->samples[mov->track->sample_count++];
	sample->sample_description_index = 1;
	sample->bytes = bytes;
	sample->flags = flags;
    sample->data = NULL;
	sample->pts = pts;
	sample->dts = dts;
	
	sample->offset = mov_buffer_tell(&mov->io);
	mov_buffer_write(&mov->io, data, bytes);

	writer->mdat_size += bytes; // update media data size
	return mov_buffer_error(&mov->io);
}

int mov_writer_add_audio(struct mov_writer_t* writer, uint8_t object, int channel_count, int bits_per_sample, int sample_rate, const void* extra_data, size_t extra_data_size)
{
	struct mov_t* mov;
	struct mov_track_t* track;

    mov = &writer->mov;
    track = mov_add_track(mov);
    if (NULL == track)
        return -ENOMEM;

    if (0 != mov_add_audio(track, &mov->mvhd, 1000, object, channel_count, bits_per_sample, sample_rate, extra_data, extra_data_size))
        return -ENOMEM;

    mov->mvhd.next_track_ID++;
    return mov->track_count++;
}

int mov_writer_add_video(struct mov_writer_t* writer, uint8_t object, int width, int height, const void* extra_data, size_t extra_data_size)
{
	struct mov_t* mov;
	struct mov_track_t* track;

    mov = &writer->mov;
    track = mov_add_track(mov);
    if (NULL == track)
        return -ENOMEM;

    if (0 != mov_add_video(track, &mov->mvhd, 1000, object, width, height, extra_data, extra_data_size))
        return -ENOMEM;

    mov->mvhd.next_track_ID++;
    return mov->track_count++;
}

int mov_writer_add_subtitle(struct mov_writer_t* writer, uint8_t object, const void* extra_data, size_t extra_data_size)
{
	struct mov_t* mov;
	struct mov_track_t* track;

	mov = &writer->mov;
    track = mov_add_track(mov);
	if (NULL == track)
		return -ENOMEM;

    if (0 != mov_add_subtitle(track, &mov->mvhd, 1000, object, extra_data, extra_data_size))
        return -ENOMEM;

    mov->mvhd.next_track_ID++;
	return mov->track_count++;
}
