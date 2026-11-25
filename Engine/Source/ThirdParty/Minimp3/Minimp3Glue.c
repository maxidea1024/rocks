/* Rocks glue for minimp3 (Docs/Design/AudioSystem section 3.26). It compiles the header-only
   implementation once and exposes the static tag helpers of minimp3_ex.h under Rocks names, so
   AudioTools can decode frame by frame without the allocating minimp3_ex load functions. */
#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"

/* The Xing/LAME tag reader walks the tag fields (frame count, byte count, 100-byte TOC, scale,
   LAME extension) before it checks the frame size, so it reads up to about 120 bytes past the side
   information. The frame is copied into a zero-filled buffer with this much room after it. */
#define ROCKS_MINIMP3_TAG_SLACK 512
#define ROCKS_MINIMP3_TAG_BUFFER (MAX_FREE_FORMAT_FRAME_SIZE + ROCKS_MINIMP3_TAG_SLACK)

/* Skips ID3v2 at the front and ID3v1/APEv2 at the end. Returns the remaining byte count and stores
   the offset of the first byte after ID3v2 in *begin. */
size_t RocksMinimp3SkipTags(const uint8_t* buffer, size_t size, size_t* begin) {
    const uint8_t* start = buffer;
    mp3dec_skip_id3(&start, &size);
    *begin = (size_t)(start - buffer);
    return size;
}

/* Reads a Xing/Info (LAME) tag from one whole frame: 1 with the frame count, delay and padding
   (both include the 529-sample decoder delay, padding may be negative), -1 for a tag without the
   frame count, 0 for none. Frames larger than the free-format limit are reported as having no tag.
 */
int RocksMinimp3CheckVbrTag(const uint8_t* frame, int frameBytes, uint32_t* frames, int* delay,
                            int* padding) {
    uint8_t padded[ROCKS_MINIMP3_TAG_BUFFER];
    if (frameBytes <= HDR_SIZE || frameBytes > MAX_FREE_FORMAT_FRAME_SIZE) {
        return 0;
    }

    memset(padded, 0, sizeof(padded));
    memcpy(padded, frame, (size_t)frameBytes);
    return mp3dec_check_vbrtag(padded, frameBytes, frames, delay, padding);
}
