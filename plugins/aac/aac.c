/*
    DeaDBeeF - ultimate music player for GNU/Linux systems with X11
    Copyright (C) 2009-2012 Alexey Yakovenko <waker@users.sourceforge.net>

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.
    
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <neaacdec.h>
#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif
#include <stdlib.h>
#include <math.h>
#include "../../deadbeef.h"
#include "aac_parser.h"

#include "mp4ff.h"

#define min(x,y) ((x)<(y)?(x):(y))
#define max(x,y) ((x)>(y)?(x):(y))

#define trace(...) { fprintf(stderr, __VA_ARGS__); }
//#define trace(fmt,...)

static DB_decoder_t plugin;
static DB_functions_t *deadbeef;

//#define AAC_BUFFER_SIZE 50000
#define AAC_BUFFER_SIZE (FAAD_MIN_STREAMSIZE * 16)
#define OUT_BUFFER_SIZE 100000

#define MP4FILE mp4ff_t *
#define MP4FILE_CB mp4ff_callback_t


// aac channel mapping
// 0: Defined in AOT Specifc Config
// 1: 1 channel: front-center
// 2: 2 channels: front-left, front-right
// 3: 3 channels: front-center, front-left, front-right
// 4: 4 channels: front-center, front-left, front-right, back-center
// 5: 5 channels: front-center, front-left, front-right, back-left, back-right
// 6: 6 channels: front-center, front-left, front-right, back-left, back-right, LFE-channel
// 7: 8 channels: front-center, front-left, front-right, side-left, side-right, back-left, back-right, LFE-channel
// 8-15: Reserved


typedef struct {
    DB_fileinfo_t info;
    NeAACDecHandle dec;
    DB_FILE *file;
    MP4FILE mp4;
    MP4FILE_CB mp4reader;
    NeAACDecFrameInfo frame_info; // last frame info
    int mp4track;
    int mp4samples;
    int mp4sample;
    int mp4framesize;
    int skipsamples;
    int startsample;
    int endsample;
    int currentsample;
    char buffer[AAC_BUFFER_SIZE];
    int remaining;
    char out_buffer[OUT_BUFFER_SIZE];
    int out_remaining;
    int num_errors;
    char *samplebuffer;
    int remap[10];
    int noremap;
    int eof;
    int junk;
} aac_info_t;

// allocate codec control structure
static DB_fileinfo_t *
aac_open (uint32_t hints) {
    DB_fileinfo_t *_info = malloc (sizeof (aac_info_t));
    aac_info_t *info = (aac_info_t *)_info;
    memset (info, 0, sizeof (aac_info_t));
    return _info;
}

static uint32_t
aac_fs_read (void *user_data, void *buffer, uint32_t length) {
//    trace ("aac_fs_read %d\n", length);
    aac_info_t *info = user_data;
    return deadbeef->fread (buffer, 1, length, info->file);
}
static uint32_t
aac_fs_seek (void *user_data, uint64_t position) {
    aac_info_t *info = user_data;
//    trace ("aac_fs_seek %lld (%lld)\n", position, position + info->junk);
    return deadbeef->fseek (info->file, position+info->junk, SEEK_SET);
}


static int
parse_aac_stream(DB_FILE *fp, int *psamplerate, int *pchannels, float *pduration, int *ptotalsamples)
{
    size_t framepos = deadbeef->ftell (fp);
    size_t initfpos = framepos;
    int firstframepos = -1;
    int fsize = -1;
    int offs = 0;
    if (!fp->vfs->is_streaming ()) {
        int skip = deadbeef->junk_get_leading_size (fp);
        if (skip >= 0) {
            deadbeef->fseek (fp, skip, SEEK_SET);
        }
        int offs = deadbeef->ftell (fp);
        fsize = deadbeef->fgetlength (fp);
        if (skip > 0) {
            fsize -= skip;
        }
    }

    uint8_t buf[ADTS_HEADER_SIZE*8];

    int nsamples = 0;
    int stream_sr = 0;
    int stream_ch = 0;

    int eof = 0;
    int bufsize = 0;
    int remaining = 0;

    int frame = 0;
    int scanframes = 1000;
    if (fp->vfs->is_streaming ()) {
        scanframes = 1;
    }

    do {
        int size = sizeof (buf) - bufsize;
        if (deadbeef->fread (buf + bufsize, 1, size, fp) != size) {
            trace ("parse_aac_stream: eof\n");
            break;
        }
        bufsize = sizeof (buf);

        int channels, samplerate, bitrate, samples;
        size = aac_sync (buf, &channels, &samplerate, &bitrate, &samples);
        if (size == 0) {
            memmove (buf, buf+1, sizeof (buf)-1);
            bufsize--;
//            trace ("aac_sync fail, framepos: %d\n", framepos);
            if (deadbeef->ftell (fp) - initfpos > 2000) { // how many is enough to make sure?
                break;
            }
            framepos++;
            continue;
        }
        else {
            trace ("aac: frame #%d sync: %dch %d %d %d %d\n", frame, channels, samplerate, bitrate, samples, size);
            frame++;
            nsamples += samples;
            if (!stream_sr) {
                stream_sr = samplerate;
            }
            if (!stream_ch) {
                stream_ch = channels;
            }
            if (firstframepos == -1) {
                firstframepos = framepos;
            }
//            if (fp->vfs->streaming) {
//                *psamplerate = stream_sr;
//                *pchannels = stream_ch;
//            }
            framepos += size;
            if (deadbeef->fseek (fp, size-(int)sizeof(buf), SEEK_CUR) == -1) {
                trace ("parse_aac_stream: invalid seek %d\n", size-sizeof(buf));
                break;
            }
            bufsize = 0;
        }
    } while (ptotalsamples || frame < scanframes);

    if (!frame || !stream_sr || !nsamples) {
        return -1;
    }

    *psamplerate = stream_sr;

    *pchannels = stream_ch;

    if (ptotalsamples) {
        *ptotalsamples = nsamples;
        *pduration = nsamples / (float)stream_sr;
        trace ("aac: duration=%f (%d samples @ %d Hz), fsize=%d, nframes=%d\n", *pduration, *ptotalsamples, stream_sr, fsize, frame);
    }
    else {
        int pos = deadbeef->ftell (fp);
        int totalsamples = (double)fsize / (pos-offs) * nsamples;
        *pduration = totalsamples / (float)stream_sr;
        trace ("aac: duration=%f (%d samples @ %d Hz), fsize=%d\n", *pduration, totalsamples, stream_sr, fsize);
    }

    if (*psamplerate <= 24000) {
        *psamplerate *= 2;
        if (ptotalsamples) {
            *ptotalsamples *= 2;
        }
    }
    return firstframepos;
}

static int
mp4_track_get_info(mp4ff_t *mp4, int track, float *duration, int *samplerate, int *channels, int *totalsamples) {
    int sr = -1;
    unsigned char*  buff = 0;
    unsigned int    buff_size = 0;
    mp4AudioSpecificConfig mp4ASC;
    mp4ff_get_decoder_config(mp4, track, &buff, &buff_size);
    if (buff) {
        int rc = AudioSpecificConfig(buff, buff_size, &mp4ASC);
        sr = mp4ASC.samplingFrequency;
        if(rc < 0) {
            free (buff);
            trace ("aac: AudioSpecificConfig returned result=%d\n", rc);
            return -1;
        }
    }

    unsigned long srate;
    unsigned char ch;
    int samples;

    // init mp4 decoding
    NeAACDecHandle dec = NeAACDecOpen ();
    if (NeAACDecInit2(dec, buff, buff_size, &srate, &ch) < 0) {
        trace ("NeAACDecInit2 returned error\n");
        goto error;
    }
    *samplerate = srate;
    *channels = ch;
    samples = (int64_t)mp4ff_num_samples(mp4, track);
    NeAACDecConfigurationPtr conf = NeAACDecGetCurrentConfiguration (dec);
    conf->dontUpSampleImplicitSBR = 1;
    NeAACDecSetConfiguration (dec, conf);
    int mp4framesize = mp4ASC.frameLengthFlag == 1 ? 960 : 1024;
    if (mp4ASC.sbr_present_flag == 1) {
        mp4framesize *= 2;
    }
    samples *= mp4framesize;

    *duration = (float)samples / (*samplerate);
    
    NeAACDecClose (dec);

    if (totalsamples) {
        *totalsamples = samples;
    }
    return 0;
error:
    if (dec) {
        NeAACDecClose (dec);
    }
    free (buff);
    return -1;
}

// returns -1 for error, 0 for aac
int
aac_probe (DB_FILE *fp, float *duration, int *samplerate, int *channels, int *totalsamples) {

    deadbeef->rewind (fp);
    if (parse_aac_stream (fp, samplerate, channels, duration, totalsamples) == -1) {
        trace ("aac stream not found\n");
        return -1;
    }
    trace ("found aac stream\n");
    return 0;
}


static int
aac_init (DB_fileinfo_t *_info, DB_playItem_t *it) {
    aac_info_t *info = (aac_info_t *)_info;

    deadbeef->pl_lock ();
    info->file = deadbeef->fopen (deadbeef->pl_find_meta (it, ":URI"));
    deadbeef->pl_unlock ();
    if (!info->file) {
        return -1;
    }

    // probe
    float duration = -1;
    int samplerate = -1;
    int channels = -1;
    int totalsamples = -1;

    info->junk = deadbeef->junk_get_leading_size (info->file);
    if (!info->file->vfs->is_streaming ()) {
        if (info->junk >= 0) {
            deadbeef->fseek (info->file, info->junk, SEEK_SET);
        }
        else {
            info->junk = 0;
        }
    }
    else {
        deadbeef->fset_track (info->file, it);
    }

    info->mp4track = -1;
    info->mp4reader.read = aac_fs_read;
    info->mp4reader.write = NULL;
    info->mp4reader.seek = aac_fs_seek;
    info->mp4reader.truncate = NULL;
    info->mp4reader.user_data = info;

    if (!info->file->vfs->is_streaming ()) {
        trace ("aac_init: mp4ff_open_read %s\n", deadbeef->pl_find_meta (it, ":URI"));
        info->mp4 = mp4ff_open_read (&info->mp4reader);
        if (info->mp4) {
            int ntracks = mp4ff_total_tracks (info->mp4);
            for (int i = 0; i < ntracks; i++) {
                if (mp4ff_get_track_type (info->mp4, i) != TRACK_AUDIO) {
                    continue;
                }
                int res = mp4_track_get_info (info->mp4, i, &duration, &samplerate, &channels, &totalsamples);
                if (res >= 0 && duration > 0) {
                    info->mp4track = i;
                    break;
                }
            }
            trace ("track: %d\n", info->mp4track);
            if (info->mp4track >= 0) {
                // prepare decoder
                int res = mp4_track_get_info (info->mp4, info->mp4track, &duration, &samplerate, &channels, &totalsamples);
                if (res != 0) {
                    trace ("aac: mp4_track_get_info(%d) returned error\n", info->mp4track);
                    return -1;
                }

                // init mp4 decoding
                info->mp4samples = mp4ff_num_samples(info->mp4, info->mp4track);
                info->dec = NeAACDecOpen ();
                unsigned long srate;
                unsigned char ch;
                unsigned char*  buff = 0;
                unsigned int    buff_size = 0;
                mp4AudioSpecificConfig mp4ASC;
                mp4ff_get_decoder_config (info->mp4, info->mp4track, &buff, &buff_size);
                if(buff) {
                    int rc = AudioSpecificConfig(buff, buff_size, &mp4ASC);
                    if(rc < 0) {
                        free (buff);
                        return -1;
                    }
                }
                
                if (NeAACDecInit2(info->dec, buff, buff_size, &srate, &ch) < 0) {
                    trace ("NeAACDecInit2 returned error\n");
                    free (buff);
                    return -1;
                }
                if (NeAACDecAudioSpecificConfig(buff, buff_size, &mp4ASC) < 0) {
                    return -1;
                }
                info->mp4framesize = mp4ASC.frameLengthFlag == 1 ? 960 : 1024;
                if (mp4ASC.sbr_present_flag == 1) {
                    info->mp4framesize *= 2;
                }

                if (buff) {
                    free (buff);
                }
            }
            else {
                trace ("aac: audio track not found\n");
                return -1;
            }
            trace ("aac: successfully initialized track %d\n", info->mp4track);
            _info->fmt.samplerate = samplerate;
            _info->fmt.channels = channels;
        }
        else {
            trace ("aac: looking for raw stream...\n");

            if (info->junk >= 0) {
                deadbeef->fseek (info->file, info->junk, SEEK_SET);
            }
            else {
                deadbeef->rewind (info->file);
            }
            int offs = parse_aac_stream (info->file, &samplerate, &channels, &duration, &totalsamples);
            if (offs == -1) {
                trace ("aac stream not found\n");
                return -1;
            }
            if (offs > info->junk) {
                info->junk = offs;
            }
            if (info->junk >= 0) {
                deadbeef->fseek (info->file, info->junk, SEEK_SET);
            }
            else {
                deadbeef->rewind (info->file);
            }
            trace ("found aac stream (junk: %d, offs: %d)\n", info->junk, offs);
        }

        _info->fmt.channels = channels;
        _info->fmt.samplerate = samplerate;
    }
    else {
        // sync before attempting to init
        int samplerate, channels;
        float duration;
        int offs = parse_aac_stream (info->file, &samplerate, &channels, &duration, NULL);
        if (offs < 0) {
            trace ("aac: parse_aac_stream failed\n");
            return -1;
        }
        if (offs > info->junk) {
            info->junk = offs;
        }
        trace("parse_aac_stream returned %x\n", offs);
        deadbeef->pl_replace_meta (it, "!FILETYPE", "AAC");
    }

//    duration = (float)totalsamples / samplerate;
//    deadbeef->pl_set_item_duration (it, duration);

    _info->fmt.bps = 16;
    _info->plugin = &plugin;

    if (!info->mp4) {
        trace ("NeAACDecOpen for raw stream\n");
        info->dec = NeAACDecOpen ();

        trace ("prepare for NeAACDecInit: fread %d from offs %lld\n", AAC_BUFFER_SIZE, deadbeef->ftell (info->file));
        info->remaining = deadbeef->fread (info->buffer, 1, AAC_BUFFER_SIZE, info->file);

        NeAACDecConfigurationPtr conf = NeAACDecGetCurrentConfiguration (info->dec);
//        conf->dontUpSampleImplicitSBR = 1;
        NeAACDecSetConfiguration (info->dec, conf);
        unsigned long srate;
        unsigned char ch;
        trace ("NeAACDecInit (%d bytes)\n", info->remaining);
        int consumed = NeAACDecInit (info->dec, info->buffer, info->remaining, &srate, &ch);
        trace ("NeAACDecInit returned samplerate=%d, channels=%d, consumed: %d\n", (int)srate, (int)ch, consumed);
        if (consumed < 0) {
            trace ("NeAACDecInit returned %d\n", consumed);
            return -1;
        }
        if (consumed > info->remaining) {
            trace ("NeAACDecInit consumed more than available! wtf?\n");
            return -1;
        }
        if (consumed == info->remaining) {
            info->remaining = 0;
        }
        else if (consumed > 0) {
            memmove (info->buffer, info->buffer + consumed, info->remaining - consumed);
            info->remaining -= consumed;
        }
        _info->fmt.channels = ch;
        _info->fmt.samplerate = srate;
    }

    if (!info->file->vfs->is_streaming ()) {
        if (it->endsample > 0) {
            info->startsample = it->startsample;
            info->endsample = it->endsample;
            plugin.seek_sample (_info, 0);
        }
        else {
            info->startsample = 0;
            info->endsample = totalsamples-1;
        }
    }
    trace ("totalsamples: %d, endsample: %d, samples-from-duration: %d, samplerate %d, channels %d\n", totalsamples-1, info->endsample, (int)deadbeef->pl_get_item_duration (it)*44100, _info->fmt.samplerate, _info->fmt.channels);

    for (int i = 0; i < _info->fmt.channels; i++) {
        _info->fmt.channelmask |= 1 << i;
    }
    info->noremap = 0;
    info->remap[0] = -1;
    trace ("init success\n");

    return 0;
}

static void
aac_free (DB_fileinfo_t *_info) {
    aac_info_t *info = (aac_info_t *)_info;
    if (info) {
        if (info->file) {
            deadbeef->fclose (info->file);
        }
        if (info->mp4) {
            mp4ff_close (info->mp4);
        }
        if (info->dec) {
            NeAACDecClose (info->dec);
        }
        free (info);
    }
}

static int
aac_read (DB_fileinfo_t *_info, char *bytes, int size) {
    aac_info_t *info = (aac_info_t *)_info;
    if (info->eof) {
        trace ("aac_read: received call after eof\n");
        return 0;
    }
    int samplesize = _info->fmt.channels * _info->fmt.bps / 8;
    if (!info->file->vfs->is_streaming ()) {
        if (info->currentsample + size / samplesize > info->endsample) {
            size = (info->endsample - info->currentsample + 1) * samplesize;
            if (size <= 0) {
                trace ("aac_read: eof (current=%d, total=%d)\n", info->currentsample, info->endsample);
                return 0;
            }
        }
    }

    int initsize = size;

    while (size > 0) {
        if (info->skipsamples > 0 && info->out_remaining > 0) {
            int skip = min (info->out_remaining, info->skipsamples);
            if (skip < info->out_remaining) {
                memmove (info->out_buffer, info->out_buffer + skip * samplesize, (info->out_remaining - skip) * samplesize);
            }
            info->out_remaining -= skip;
            info->skipsamples -= skip;
        }
        if (info->out_remaining > 0) {
            int n = size / samplesize;
            n = min (info->out_remaining, n);

            char *src = info->out_buffer;
            if (info->noremap) {
                memcpy (bytes, src, n * samplesize);
                bytes += n * samplesize;
                src += n * samplesize;
            }
            else {
                int i, j;
                if (info->remap[0] == -1) {
                    // build remap mtx

                    // FIXME: should build channelmask 1st; then remap based on channelmask
                    for (i = 0; i < _info->fmt.channels; i++) {
                        switch (info->frame_info.channel_position[i]) {
                        case FRONT_CHANNEL_CENTER:
                            trace ("FC->%d\n", i);
                            info->remap[2] = i;
                            break;
                        case FRONT_CHANNEL_LEFT:
                            trace ("FL->%d\n", i);
                            info->remap[0] = i;
                            break;
                        case FRONT_CHANNEL_RIGHT:
                            trace ("FR->%d\n", i);
                            info->remap[1] = i;
                            break;
                        case SIDE_CHANNEL_LEFT:
                            trace ("SL->%d\n", i);
                            info->remap[6] = i;
                            break;
                        case SIDE_CHANNEL_RIGHT:
                            trace ("SR->%d\n", i);
                            info->remap[7] = i;
                            break;
                        case BACK_CHANNEL_LEFT:
                            trace ("RL->%d\n", i);
                            info->remap[4] = i;
                            break;
                        case BACK_CHANNEL_RIGHT:
                            trace ("RR->%d\n", i);
                            info->remap[5] = i;
                            break;
                        case BACK_CHANNEL_CENTER:
                            trace ("BC->%d\n", i);
                            info->remap[8] = i;
                            break;
                        case LFE_CHANNEL:
                            trace ("LFE->%d\n", i);
                            info->remap[3] = i;
                            break;
                        default:
                            trace ("aac: unknown ch(%d)->%d\n", info->frame_info.channel_position[i], i);
                            break;
                        }
                    }
                    for (i = 0; i < _info->fmt.channels; i++) {
                        trace ("%d ", info->remap[i]);
                    }
                    trace ("\n");
                    if (info->remap[0] == -1) {
                        info->remap[0] = 0;
                    }
                    if ((_info->fmt.channels == 1 && info->remap[0] == FRONT_CHANNEL_CENTER)
                        || (_info->fmt.channels == 2 && info->remap[0] == FRONT_CHANNEL_LEFT && info->remap[1] == FRONT_CHANNEL_RIGHT)) {
                        info->noremap = 1;
                    }
                }

                for (i = 0; i < n; i++) {
                    for (j = 0; j < _info->fmt.channels; j++) {
                        ((int16_t *)bytes)[j] = ((int16_t *)src)[info->remap[j]];
                    }
                    src += samplesize;
                    bytes += samplesize;
                }
            }
            size -= n * samplesize;

            if (n == info->out_remaining) {
                info->out_remaining = 0;
            }
            else {
                memmove (info->out_buffer, src, (info->out_remaining - n) * samplesize);
                info->out_remaining -= n;
            }
            continue;
        }

        char *samples = NULL;

        if (info->mp4) {
            if (info->mp4sample >= info->mp4samples) {
                trace ("aac: finished with the last mp4sample\n");
                break;
            }
            
            unsigned char *buffer = NULL;
            int buffer_size = 0;
            int rc = mp4ff_read_sample (info->mp4, info->mp4track, info->mp4sample, &buffer, &buffer_size);
            if (rc == 0) {
                trace ("mp4ff_read_sample failed\n");
                info->eof = 1;
                break;
            }
            info->mp4sample++;
            samples = NeAACDecDecode(info->dec, &info->frame_info, buffer, buffer_size);

            if (buffer) {
                free (buffer);
            }
            if (!samples) {
                trace ("aac: NeAACDecDecode returned NULL\n");
                break;
            }
        }
        else {
            if (info->remaining < AAC_BUFFER_SIZE) {
                trace ("fread from offs %lld\n", deadbeef->ftell (info->file));
                size_t res = deadbeef->fread (info->buffer + info->remaining, 1, AAC_BUFFER_SIZE-info->remaining, info->file);
                info->remaining += res;
                trace ("remain: %d\n", info->remaining);
                if (!info->remaining) {
                    break;
                }
            }

            trace ("NeAACDecDecode %d bytes\n", info->remaining)
            samples = NeAACDecDecode (info->dec, &info->frame_info, info->buffer, info->remaining);
            trace ("samples =%p\n", samples);
            if (!samples) {
                trace ("NeAACDecDecode failed with error %s (%d), consumed=%d\n", NeAACDecGetErrorMessage(info->frame_info.error), (int)info->frame_info.error, info->frame_info.bytesconsumed);

                if (info->num_errors > 10) {
                    trace ("NeAACDecDecode failed %d times, interrupting\n", info->num_errors);
                    break;
                }
                info->num_errors++;
                info->remaining = 0;
                continue;
            }
            info->num_errors=0;
            int consumed = info->frame_info.bytesconsumed;
            if (consumed > info->remaining) {
                trace ("NeAACDecDecode consumed more than available! wtf?\n");
                break;
            }
            if (consumed == info->remaining) {
                info->remaining = 0;
            }
            else if (consumed > 0) {
                memmove (info->buffer, info->buffer + consumed, info->remaining - consumed);
                info->remaining -= consumed;
            }
        }

        if (info->frame_info.samples > 0) {
            memcpy (info->out_buffer, samples, info->frame_info.samples * 2);
            info->out_remaining = info->frame_info.samples / info->frame_info.channels;
        }
    }

    info->currentsample += (initsize-size) / samplesize;
    return initsize-size;
}

// returns -1 on error, 0 on success
int
seek_raw_aac (aac_info_t *info, int sample) {
    uint8_t buf[ADTS_HEADER_SIZE*8];

    int nsamples = 0;
    int stream_sr = 0;
    int stream_ch = 0;

    int eof = 0;
    int bufsize = 0;
    int remaining = 0;

    int frame = 0;

    int frame_samples = 0;
    int curr_sample = 0;

    do {
        curr_sample += frame_samples;
        int size = sizeof (buf) - bufsize;
        if (deadbeef->fread (buf + bufsize, 1, size, info->file) != size) {
            trace ("seek_raw_aac: eof\n");
            break;
        }
        bufsize = sizeof (buf);

        int channels, samplerate, bitrate;
        size = aac_sync (buf, &channels, &samplerate, &bitrate, &frame_samples);
        if (size == 0) {
            memmove (buf, buf+1, sizeof (buf)-1);
            bufsize--;
            continue;
        }
        else {
            //trace ("aac: frame #%d(%d/%d) sync: %d %d %d %d %d\n", frame, curr_sample, sample, channels, samplerate, bitrate, frame_samples, size);
            frame++;
            if (deadbeef->fseek (info->file, size-(int)sizeof(buf), SEEK_CUR) == -1) {
                trace ("seek_raw_aac: invalid seek %d\n", size-sizeof(buf));
                break;
            }
            bufsize = 0;
        }
        if (samplerate <= 24000) {
            frame_samples *= 2;
        }
    } while (curr_sample + frame_samples < sample);

    if (curr_sample + frame_samples < sample) {
        return -1;
    }

    return sample - curr_sample;
}

static int
aac_seek_sample (DB_fileinfo_t *_info, int sample) {
    aac_info_t *info = (aac_info_t *)_info;

    sample += info->startsample;
    if (info->mp4) {
        int scale = info->mp4framesize;
        info->mp4sample = sample / scale;
        info->skipsamples = sample - info->mp4sample * scale;
    }
    else {
        int skip = deadbeef->junk_get_leading_size (info->file);
        if (skip >= 0) {
            deadbeef->fseek (info->file, skip, SEEK_SET);
        }
        else {
            deadbeef->fseek (info->file, 0, SEEK_SET);
        }

        int res = seek_raw_aac (info, sample);
        if (res < 0) {
            return -1;
        }
        info->skipsamples = res;
    }
    info->remaining = 0;
    info->out_remaining = 0;
    info->currentsample = sample;
    _info->readpos = (float)(info->currentsample - info->startsample) / _info->fmt.samplerate;
    return 0;
}

static int
aac_seek (DB_fileinfo_t *_info, float t) {
    return aac_seek_sample (_info, t * _info->fmt.samplerate);
}

static const char *metainfo[] = {
    "artist", "artist",
    "title", "title",
    "album", "album",
    "track", "track",
    "date", "year",
    "genre", "genre",
    "comment", "comment",
    "performer", "performer",
    "album_artist", "band",
    "writer", "composer",
    "vendor", "vendor",
    "disc", "disc",
    "compilation", "compilation",
    "totaldiscs", "numdiscs",
    "copyright", "copyright",
    "totaltracks", "numtracks",
    "tool", "tool",
    NULL
};


/* find a metadata item by name */
/* returns 0 if item found, 1 if no such item */
int32_t mp4ff_meta_find_by_name(const mp4ff_t *f, const char *item, char **value);


void
aac_load_tags (DB_playItem_t *it, mp4ff_t *mp4) {
    char *s = NULL;
    int got_itunes_tags = 0;

    int n = mp4ff_meta_get_num_items (mp4);
    for (int t = 0; t < n; t++)  {
        char *key = NULL;
        char *value = NULL;
        int res = mp4ff_meta_get_by_index(mp4, t, &key, &value);
        if (key && value) {
            got_itunes_tags = 1;
            if (strcasecmp (key, "cover")) {
                if (!strcasecmp (key, "replaygain_track_gain")) {
                    deadbeef->pl_set_item_replaygain (it, DDB_REPLAYGAIN_TRACKGAIN, atof (value));
                }
                else if (!strcasecmp (key, "replaygain_album_gain")) {
                    deadbeef->pl_set_item_replaygain (it, DDB_REPLAYGAIN_ALBUMGAIN, atof (value));
                }
                else if (!strcasecmp (key, "replaygain_track_peak")) {
                    deadbeef->pl_set_item_replaygain (it, DDB_REPLAYGAIN_TRACKPEAK, atof (value));
                }
                else if (!strcasecmp (key, "replaygain_album_peak")) {
                    deadbeef->pl_set_item_replaygain (it, DDB_REPLAYGAIN_ALBUMPEAK, atof (value));
                }
                else {
                    int i;
                    for (i = 0; metainfo[i]; i += 2) {
                        if (!strcasecmp (metainfo[i], key)) {
                            deadbeef->pl_add_meta (it, metainfo[i+1], value);
                            break;
                        }
                    }
                    if (!metainfo[i]) {
                        deadbeef->pl_add_meta (it, key, value);
                    }
                }
            }
        }
        if (key) {
            free (key);
        }
        if (value) {
            free (value);
        }
    }

    if (got_itunes_tags) {
        uint32_t f = deadbeef->pl_get_item_flags (it);
        f |= DDB_TAG_ITUNES;
        deadbeef->pl_set_item_flags (it, f);
    }
}


int
aac_read_metadata (DB_playItem_t *it) {
    deadbeef->pl_lock ();
    DB_FILE *fp = deadbeef->fopen (deadbeef->pl_find_meta (it, ":URI"));
    deadbeef->pl_unlock ();
    if (!fp) {
        return -1;
    }

    if (fp->vfs->is_streaming ()) {
        deadbeef->fclose (fp);
        return -1;
    }

    aac_info_t inf;
    memset (&inf, 0, sizeof (inf));
    inf.file = fp;
    inf.junk = deadbeef->junk_get_leading_size (fp);
    if (inf.junk >= 0) {
        deadbeef->fseek (inf.file, inf.junk, SEEK_SET);
    }
    else {
        inf.junk = 0;
    }

    MP4FILE_CB cb = {
        .read = aac_fs_read,
        .write = NULL,
        .seek = aac_fs_seek,
        .truncate = NULL,
        .user_data = &inf
    };

    deadbeef->pl_delete_all_meta (it);

    mp4ff_t *mp4 = mp4ff_open_read (&cb);
    if (mp4) {
        aac_load_tags (it, mp4);
        mp4ff_close (mp4);
    }
    /*int apeerr = */deadbeef->junk_apev2_read (it, fp);
    /*int v2err = */deadbeef->junk_id3v2_read (it, fp);
    /*int v1err = */deadbeef->junk_id3v1_read (it, fp);
    deadbeef->fclose (fp);
    return 0;
}

typedef struct {
    char *title;
    int32_t startsample;
    int32_t endsample;
} aac_chapter_t;

static aac_chapter_t *
aac_load_itunes_chapters (mp4ff_t *mp4, /* out */ int *num_chapters, int samplerate) {
    *num_chapters = 0;
    int i_entry_count = mp4ff_chap_get_num_tracks (mp4);
    int i_tracks = mp4ff_total_tracks (mp4);
    int i, j;
    for( i = 0; i < i_entry_count; i++ )
    {
        for( j = 0; j < i_tracks; j++ )
        {
            int32_t tt = mp4ff_get_track_type (mp4, j);
            trace ("aac: i_tracks=%d found track id=%d type=%d (expected %d %d)\n", i_tracks, mp4ff_get_track_id (mp4, j), mp4ff_get_track_type (mp4, j), mp4ff_chap_get_track_id (mp4, i), TRACK_TEXT);
            if(mp4ff_chap_get_track_id (mp4, i)  == mp4ff_get_track_id (mp4, j) &&
                    mp4ff_get_track_type (mp4, j) == TRACK_TEXT) {
                trace ("aac: found subt track\n");
                break;
            }
        }
        if( j < i_tracks )
        {
            int i_sample_count = mp4ff_num_samples (mp4, j);
            int i_sample;

            aac_chapter_t *chapters = malloc (sizeof (aac_chapter_t) * i_sample_count);
            memset (chapters, 0, sizeof (aac_chapter_t) * i_sample_count);
            *num_chapters = 0;

            int64_t total_dur = 0;
            int64_t curr_sample = 0;
            for( i_sample = 0; i_sample < i_sample_count; i_sample++ )
            {
                int32_t dur = (int64_t)1000 * mp4ff_get_sample_duration (mp4, j, i_sample) / mp4ff_time_scale (mp4, j); // milliseconds
                total_dur += dur;
                trace ("dur: %d (%f min)\n", dur, dur / 1000.f / 60.f);
                unsigned char *buffer = NULL;
                int buffer_size = 0;

                int rc = mp4ff_read_sample (mp4, j, i_sample, &buffer, &buffer_size);

                if (rc == 0 || !buffer) {
                    continue;
                }
                int len = (buffer[0] << 8) | buffer[1];
                len = min (len, buffer_size - 2);
                chapters[*num_chapters].title = strndup (&buffer[2], len);
                chapters[*num_chapters].startsample = curr_sample;
                curr_sample += (int64_t)dur * samplerate / 1000.f;
                chapters[*num_chapters].endsample = curr_sample - 1;
                trace ("aac: chapter %d: %s, s=%d e=%d\n", *num_chapters, chapters[*num_chapters].title, chapters[*num_chapters].startsample, chapters[*num_chapters].endsample);
                if (buffer) {
                    free (buffer);
                }
                (*num_chapters)++;
            }
            trace ("aac: total dur: %lld (%f min)\n", total_dur, total_dur / 1000.f / 60.f);
            return chapters;
        }
    }
    return NULL;
}

static DB_playItem_t *
aac_insert_with_chapters (ddb_playlist_t *plt, DB_playItem_t *after, DB_playItem_t *origin, aac_chapter_t *chapters, int num_chapters, int totalsamples, int samplerate) {
    deadbeef->pl_lock ();
    DB_playItem_t *ins = after;
    for (int i = 0; i < num_chapters; i++) {
        const char *uri = deadbeef->pl_find_meta_raw (origin, ":URI");
        const char *dec = deadbeef->pl_find_meta_raw (origin, ":DECODER");
        const char *ftype= "MP4 AAC";//pl_find_meta_raw (origin, ":FILETYPE");

        DB_playItem_t *it = deadbeef->pl_item_alloc_init (uri, dec);
        deadbeef->pl_set_meta_int (it, ":TRACKNUM", i);
        deadbeef->pl_set_meta_int (it, "TRACK", i);
        deadbeef->pl_add_meta (it, "title", chapters[i].title);
        it->startsample = chapters[i].startsample;
        it->endsample = chapters[i].endsample;
        deadbeef->pl_replace_meta (it, ":FILETYPE", ftype);
        deadbeef->plt_set_item_duration (plt, it, (float)(it->endsample - it->startsample + 1) / samplerate);
        after = deadbeef->plt_insert_item (plt, after, it);
        deadbeef->pl_item_unref (it);
    }
    deadbeef->pl_item_ref (after);
    
    DB_playItem_t *first = deadbeef->pl_get_next (ins, PL_MAIN);
    
    if (!first) {
        first = deadbeef->plt_get_first (plt, PL_MAIN);
    }

    if (!first) {
        deadbeef->pl_unlock ();
        return NULL;
    }
    trace ("aac: split by chapters success\n");
    // copy metadata from embedded tags
    uint32_t f = deadbeef->pl_get_item_flags (origin);
    f |= DDB_IS_SUBTRACK;
    deadbeef->pl_set_item_flags (origin, f);
    deadbeef->pl_items_copy_junk (origin, first, after);
    deadbeef->pl_item_unref (first);

    deadbeef->pl_unlock ();
    return after;
}

static DB_playItem_t *
aac_insert (ddb_playlist_t *plt, DB_playItem_t *after, const char *fname) {
    trace ("adding %s\n", fname);
    DB_FILE *fp = deadbeef->fopen (fname);
    if (!fp) {
        trace ("not found\n");
        return NULL;
    }
    aac_info_t info = {0};
    info.junk = deadbeef->junk_get_leading_size (fp);
    if (info.junk >= 0) {
        trace ("junk: %d\n", info.junk);
        deadbeef->fseek (fp, info.junk, SEEK_SET);
    }
    else {
        info.junk = 0;
    }

    const char *ftype = NULL;
    float duration = -1;
    int totalsamples = 0;
    int samplerate = 0;
    int channels = 0;

    int mp4track = -1;
    MP4FILE mp4 = NULL;

    if (fp->vfs->is_streaming ()) {
        trace ("streaming aac (%s)\n", fname);
        ftype = "RAW AAC";
    }
    else {

        // slowwww!
        info.file = fp;
        MP4FILE_CB cb = {
            .read = aac_fs_read,
            .write = NULL,
            .seek = aac_fs_seek,
            .truncate = NULL,
            .user_data = &info
        };
        mp4ff_t *mp4 = mp4ff_open_read (&cb);
        if (mp4) {
            int ntracks = mp4ff_total_tracks (mp4);
            trace ("aac: numtracks=%d\n", ntracks);

/// TODO: this is needed for multi-track files, but we disable this for now
///            // calculate number of aac subtracks
///            int num_aac_tracks = 0;
///            for (int i = 0; i < ntracks; i++) {
///                if (mp4ff_get_track_type (mp4, i) == TRACK_AUDIO)
///                    num_aac_tracks++; // FIXME: will count alac too
///                }
///            }

/// show how many audio tracks is in the mp4
///            int naudio = 0;
///            for (int i = 0; i < ntracks; i++) {
///                if (mp4ff_get_track_type (mp4, i) == TRACK_AUDIO) {
///                    naudio++;
///                }
///            }
///            trace ("found %d audio tracks\n", naudio);

            for (int i = 0; i < ntracks; i++) {
                if (mp4ff_get_track_type (mp4, i) != TRACK_AUDIO) {
                    continue;
                }
                int res = mp4_track_get_info (mp4, i, &duration, &samplerate, &channels, &totalsamples);
                if (res >= 0 && duration > 0) {
                    trace ("aac: found audio track %d (duration=%f, totalsamples=%d)\n", i, duration, totalsamples);

                    int num_chapters;
                    aac_chapter_t *chapters = NULL;
                    if (mp4ff_chap_get_num_tracks(mp4) > 0) {
                        chapters = aac_load_itunes_chapters (mp4, &num_chapters, samplerate);
                    }

                    DB_playItem_t *it = deadbeef->pl_item_alloc_init (fname, plugin.plugin.id);
                    deadbeef->pl_add_meta (it, ":FILETYPE", ftype);
                    deadbeef->pl_set_meta_int (it, ":TRACKNUM", i);
                    deadbeef->plt_set_item_duration (plt, it, duration);
                    aac_load_tags (it, mp4);
                    int apeerr = deadbeef->junk_apev2_read (it, fp);
                    int v2err = deadbeef->junk_id3v2_read (it, fp);
                    int v1err = deadbeef->junk_id3v1_read (it, fp);

                    int64_t fsize = deadbeef->fgetlength (fp);

                    char s[100];
                    snprintf (s, sizeof (s), "%lld", fsize);
                    deadbeef->pl_add_meta (it, ":FILE_SIZE", s);
                    deadbeef->pl_add_meta (it, ":BPS", "16");
                    snprintf (s, sizeof (s), "%d", channels);
                    deadbeef->pl_add_meta (it, ":CHANNELS", s);
                    snprintf (s, sizeof (s), "%d", samplerate);
                    deadbeef->pl_add_meta (it, ":SAMPLERATE", s);
                    int br = (int)roundf(fsize / duration * 8 / 1000);
                    snprintf (s, sizeof (s), "%d", br);
                    deadbeef->pl_add_meta (it, ":BITRATE", s);

                    // embedded chapters
                    deadbeef->pl_lock (); // FIXME: is it needed?
                    if (chapters && num_chapters > 0) {
                        DB_playItem_t *cue = aac_insert_with_chapters (plt, after, it, chapters, num_chapters, totalsamples, samplerate);
                        for (int n = 0; n < num_chapters; n++) {
                            if (chapters[n].title) {
                                free (chapters[n].title);
                            }
                        }
                        free (chapters);
                        if (cue) {
                            deadbeef->pl_item_unref (it);
                            deadbeef->pl_item_unref (cue);
                            deadbeef->pl_unlock ();
                            return cue;
                        }
                    }

                    // embedded cue
                    const char *cuesheet = deadbeef->pl_find_meta (it, "cuesheet");
                    DB_playItem_t *cue = NULL;

                    if (cuesheet) {
                        cue = deadbeef->plt_insert_cue_from_buffer (plt, after, it, cuesheet, strlen (cuesheet), totalsamples, samplerate);
                        if (cue) {
                            deadbeef->pl_item_unref (it);
                            deadbeef->pl_item_unref (cue);
                            deadbeef->pl_unlock ();
                            return cue;
                        }
                    }
                    deadbeef->pl_unlock ();

                    cue  = deadbeef->plt_insert_cue (plt, after, it, totalsamples, samplerate);
                    if (cue) {
                        deadbeef->pl_item_unref (it);
                        deadbeef->pl_item_unref (cue);
                        return cue;
                    }

                    after = deadbeef->plt_insert_item (plt, after, it);
                    deadbeef->pl_item_unref (it);
                    break;
                }
            }
            ftype = "MP4 AAC";
        }
        else {
            int res = aac_probe (fp, &duration, &samplerate, &channels, &totalsamples);
            if (res == -1) {
                deadbeef->fclose (fp);
                return NULL;
            }
            ftype = "RAW AAC";
            DB_playItem_t *it = deadbeef->pl_item_alloc_init (fname, plugin.plugin.id);
            deadbeef->pl_add_meta (it, ":FILETYPE", ftype);
            deadbeef->plt_set_item_duration (plt, it, duration);
            trace ("duration: %f sec\n", duration);

            // read tags
            int apeerr = deadbeef->junk_apev2_read (it, fp);
            int v2err = deadbeef->junk_id3v2_read (it, fp);
            int v1err = deadbeef->junk_id3v1_read (it, fp);

            int64_t fsize = deadbeef->fgetlength (fp);

            deadbeef->fclose (fp);

            if (duration > 0) {
                char s[100];
                snprintf (s, sizeof (s), "%lld", fsize);
                deadbeef->pl_add_meta (it, ":FILE_SIZE", s);
                deadbeef->pl_add_meta (it, ":BPS", "16");
                snprintf (s, sizeof (s), "%d", channels);
                deadbeef->pl_add_meta (it, ":CHANNELS", s);
                snprintf (s, sizeof (s), "%d", samplerate);
                deadbeef->pl_add_meta (it, ":SAMPLERATE", s);
                int br = (int)roundf(fsize / duration * 8 / 1000);
                snprintf (s, sizeof (s), "%d", br);
                deadbeef->pl_add_meta (it, ":BITRATE", s);
                // embedded cue
                deadbeef->pl_lock ();
                const char *cuesheet = deadbeef->pl_find_meta (it, "cuesheet");
                DB_playItem_t *cue = NULL;

                if (cuesheet) {
                    cue = deadbeef->plt_insert_cue_from_buffer (plt, after, it, cuesheet, strlen (cuesheet), totalsamples, samplerate);
                    if (cue) {
                        deadbeef->pl_item_unref (it);
                        deadbeef->pl_item_unref (cue);
                        deadbeef->pl_unlock ();
                        return cue;
                    }
                }
                deadbeef->pl_unlock ();

                cue  = deadbeef->plt_insert_cue (plt, after, it, totalsamples, samplerate);
                if (cue) {
                    deadbeef->pl_item_unref (it);
                    deadbeef->pl_item_unref (cue);
                    return cue;
                }
            }

            after = deadbeef->plt_insert_item (plt, after, it);
            deadbeef->pl_item_unref (it);
        }
    }

    return after;
}

static const char * exts[] = { "aac", "mp4", "m4a", "m4b", NULL };

// define plugin interface
static DB_decoder_t plugin = {
    .plugin.api_vmajor = 1,
    .plugin.api_vminor = 0,
    .plugin.version_major = 1,
    .plugin.version_minor = 0,
    .plugin.type = DB_PLUGIN_DECODER,
    .plugin.id = "aac",
    .plugin.name = "AAC player",
    .plugin.descr = "plays aac files, supports raw aac files, as well as mp4 container",
    .plugin.copyright = 
        "Copyright (C) 2009-2012 Alexey Yakovenko <waker@users.sourceforge.net>\n"
        "\n"
        "Uses modified libmp4ff (C) 2003-2005 M. Bakker, Nero AG, http://www.nero.com\n"
        "\n"
        "This program is free software; you can redistribute it and/or\n"
        "modify it under the terms of the GNU General Public License\n"
        "as published by the Free Software Foundation; either version 2\n"
        "of the License, or (at your option) any later version.\n"
        "\n"
        "This program is distributed in the hope that it will be useful,\n"
        "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
        "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
        "GNU General Public License for more details.\n"
        "\n"
        "You should have received a copy of the GNU General Public License\n"
        "along with this program; if not, write to the Free Software\n"
        "Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.\n"
    ,
    .plugin.website = "http://deadbeef.sf.net",
    .open = aac_open,
    .init = aac_init,
    .free = aac_free,
    .read = aac_read,
    .seek = aac_seek,
    .seek_sample = aac_seek_sample,
    .insert = aac_insert,
    .read_metadata = aac_read_metadata,
    .exts = exts,
};

DB_plugin_t *
aac_load (DB_functions_t *api) {
    deadbeef = api;
    return DB_PLUGIN (&plugin);
}
