#define WIN32_LEAN_AND_MEAN
#define boolean windows_boolean
#include <windows.h>
#include <mmsystem.h>
#undef boolean

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "z_zone.h"

#include "i_system.h"
#include "i_sound.h"
#include "w_wad.h"

#include "doomdef.h"

#define SAMPLECOUNT 512
#define NUM_CHANNELS 8
#define BUFMUL 4
#define MIXBUFFER_BYTES (SAMPLECOUNT * BUFMUL)
#define SAMPLERATE 11025
#define AUDIO_BUFFER_COUNT 4
#define MIDI_TICKS_PER_QUARTER 70
#define MUSIC_ALIAS "doommusic"
#define MUSIC_SONG_LIMIT 8

static int lengths[NUMSFX];
static signed short mixbuffer[MIXBUFFER_BYTES];
static unsigned int channelstep[NUM_CHANNELS];
static unsigned int channelstepremainder[NUM_CHANNELS];
static unsigned char *channels[NUM_CHANNELS];
static unsigned char *channelsend[NUM_CHANNELS];
static int channelstart[NUM_CHANNELS];
static int channelhandles[NUM_CHANNELS];
static int channelids[NUM_CHANNELS];
static int steptable[256];
static int vol_lookup[128 * 256];
static int *channelleftvol_lookup[NUM_CHANNELS];
static int *channelrightvol_lookup[NUM_CHANNELS];

static HWAVEOUT sound_device;
static WAVEHDR sound_headers[AUDIO_BUFFER_COUNT];
static unsigned char sound_buffers[AUDIO_BUFFER_COUNT][MIXBUFFER_BYTES];
static int sound_buffer_ready[AUDIO_BUFFER_COUNT];
static int sound_initialized;
static int sound_thread_running;
static HANDLE sound_thread;
static HANDLE sound_shutdown_event;
static HANDLE sound_wake_event;
static DWORD sound_thread_id;
static CRITICAL_SECTION sound_lock;
static int sound_lock_initialized;
static unsigned char *sfx_wave_data[NUMSFX];
static DWORD sfx_wave_size[NUMSFX];
static int current_sound_handle;
static int current_sound_endtic;
typedef struct
{
    int used;
    unsigned char *data;
    size_t length;
} music_song_t;

static music_song_t music_songs[MUSIC_SONG_LIMIT];
static int music_initialized;
static int music_current_handle;
static int music_looping;
static int music_paused;
static int music_stop_request;
static int music_shutdown_request;
static int music_thread_running;
static HANDLE music_thread;
static HANDLE music_wake_event;
static HMIDIOUT music_out_device;

typedef struct
{
    unsigned char *data;
    size_t length;
    size_t capacity;
} midi_buffer_t;

static unsigned short MusReadLE16(const unsigned char *data)
{
    return (unsigned short)(data[0] | (data[1] << 8));
}

static int MixerVolumeFromDoom(int volume)
{
    if (volume < 0)
        return 0;

    if (volume <= 15)
        volume *= 8;

    if (volume > 127)
        volume = 127;

    return volume;
}

static int MidiChannelForMus(int mus_channel)
{
    return mus_channel;
}

static int MidiEnsureCapacity(midi_buffer_t *buffer, size_t extra)
{
    unsigned char *new_data;
    size_t new_capacity;

    if (buffer->length + extra <= buffer->capacity)
        return 1;

    new_capacity = buffer->capacity ? buffer->capacity : 1024;
    while (new_capacity < buffer->length + extra)
        new_capacity *= 2;

    new_data = (unsigned char *)realloc(buffer->data, new_capacity);
    if (!new_data)
        return 0;

    buffer->data = new_data;
    buffer->capacity = new_capacity;
    return 1;
}

static int MidiAppendByte(midi_buffer_t *buffer, unsigned char value)
{
    if (!MidiEnsureCapacity(buffer, 1))
        return 0;

    buffer->data[buffer->length++] = value;
    return 1;
}

static int MidiAppendData(midi_buffer_t *buffer, const void *data, size_t length)
{
    if (!MidiEnsureCapacity(buffer, length))
        return 0;

    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    return 1;
}

static int MidiAppendBE16(midi_buffer_t *buffer, unsigned short value)
{
    unsigned char bytes[2];

    bytes[0] = (unsigned char)((value >> 8) & 0xff);
    bytes[1] = (unsigned char)(value & 0xff);
    return MidiAppendData(buffer, bytes, sizeof(bytes));
}

static int MidiAppendBE32(midi_buffer_t *buffer, unsigned long value)
{
    unsigned char bytes[4];

    bytes[0] = (unsigned char)((value >> 24) & 0xff);
    bytes[1] = (unsigned char)((value >> 16) & 0xff);
    bytes[2] = (unsigned char)((value >> 8) & 0xff);
    bytes[3] = (unsigned char)(value & 0xff);
    return MidiAppendData(buffer, bytes, sizeof(bytes));
}

static int MidiAppendVarLen(midi_buffer_t *buffer, unsigned long value)
{
    unsigned char bytes[5];
    int count;

    count = 0;
    bytes[count++] = (unsigned char)(value & 0x7f);
    value >>= 7;

    while (value)
    {
        bytes[count++] = (unsigned char)((value & 0x7f) | 0x80);
        value >>= 7;
    }

    while (count > 0)
    {
        if (!MidiAppendByte(buffer, bytes[--count]))
            return 0;
    }

    return 1;
}

static int MidiAppendShortEvent(midi_buffer_t *buffer,
                                unsigned long delta,
                                unsigned char status,
                                unsigned char data1,
                                unsigned char data2)
{
    if (!MidiAppendVarLen(buffer, delta))
        return 0;
    if (!MidiAppendByte(buffer, status))
        return 0;
    if (!MidiAppendByte(buffer, data1))
        return 0;
    if (!MidiAppendByte(buffer, data2))
        return 0;
    return 1;
}

static int MidiAppendProgramChange(midi_buffer_t *buffer,
                                   unsigned long delta,
                                   unsigned char status,
                                   unsigned char program)
{
    if (!MidiAppendVarLen(buffer, delta))
        return 0;
    if (!MidiAppendByte(buffer, status))
        return 0;
    if (!MidiAppendByte(buffer, program))
        return 0;
    return 1;
}

static int MidiAllocateChannel(const int channel_map[16])
{
    int i;
    int max_channel;
    int result;

    max_channel = -1;
    for (i = 0; i < 16; ++i)
        if (channel_map[i] > max_channel)
            max_channel = channel_map[i];

    result = max_channel + 1;
    if (result == 9)
        ++result;

    return result;
}

static int MidiWriteQueuedTime(midi_buffer_t *buffer, unsigned long *queued_time)
{
    unsigned long buffer_value;

    buffer_value = *queued_time & 0x7f;

    while ((*queued_time >>= 7) != 0)
    {
        buffer_value <<= 8;
        buffer_value |= ((*queued_time & 0x7f) | 0x80);
    }

    for (;;)
    {
        if (!MidiAppendByte(buffer, (unsigned char)(buffer_value & 0xff)))
            return 0;

        if ((buffer_value & 0x80) != 0)
            buffer_value >>= 8;
        else
        {
            *queued_time = 0;
            return 1;
        }
    }
}

static int MidiWritePressKey(midi_buffer_t *buffer,
                             unsigned long *queued_time,
                             unsigned char channel,
                             unsigned char key,
                             unsigned char velocity)
{
    if (!MidiWriteQueuedTime(buffer, queued_time))
        return 0;
    if (!MidiAppendByte(buffer, (unsigned char)(0x90 | channel)))
        return 0;
    if (!MidiAppendByte(buffer, (unsigned char)(key & 0x7f)))
        return 0;
    if (!MidiAppendByte(buffer, (unsigned char)(velocity & 0x7f)))
        return 0;
    return 1;
}

static int MidiWriteReleaseKey(midi_buffer_t *buffer,
                               unsigned long *queued_time,
                               unsigned char channel,
                               unsigned char key)
{
    if (!MidiWriteQueuedTime(buffer, queued_time))
        return 0;
    if (!MidiAppendByte(buffer, (unsigned char)(0x80 | channel)))
        return 0;
    if (!MidiAppendByte(buffer, (unsigned char)(key & 0x7f)))
        return 0;
    if (!MidiAppendByte(buffer, 0x00))
        return 0;
    return 1;
}

static int MidiWritePitchWheel(midi_buffer_t *buffer,
                               unsigned long *queued_time,
                               unsigned char channel,
                               unsigned short wheel)
{
    if (!MidiWriteQueuedTime(buffer, queued_time))
        return 0;
    if (!MidiAppendByte(buffer, (unsigned char)(0xe0 | channel)))
        return 0;
    if (!MidiAppendByte(buffer, (unsigned char)(wheel & 0x7f)))
        return 0;
    if (!MidiAppendByte(buffer, (unsigned char)((wheel >> 7) & 0x7f)))
        return 0;
    return 1;
}

static int MidiWriteControllerValued(midi_buffer_t *buffer,
                                     unsigned long *queued_time,
                                     unsigned char channel,
                                     unsigned char controller,
                                     unsigned char value)
{
    if (!MidiWriteQueuedTime(buffer, queued_time))
        return 0;
    if (!MidiAppendByte(buffer, (unsigned char)(0xb0 | channel)))
        return 0;
    if (!MidiAppendByte(buffer, controller))
        return 0;
    if (!MidiAppendByte(buffer, (unsigned char)((value & 0x80) ? 0x7f : value)))
        return 0;
    return 1;
}

static int MidiWriteControllerValueless(midi_buffer_t *buffer,
                                        unsigned long *queued_time,
                                        unsigned char channel,
                                        unsigned char controller)
{
    return MidiWriteControllerValued(buffer, queued_time, channel, controller, 0);
}

static int MidiWritePatchChange(midi_buffer_t *buffer,
                                unsigned long *queued_time,
                                unsigned char channel,
                                unsigned char patch)
{
    if (!MidiWriteQueuedTime(buffer, queued_time))
        return 0;
    if (!MidiAppendByte(buffer, (unsigned char)(0xc0 | channel)))
        return 0;
    if (!MidiAppendByte(buffer, (unsigned char)(patch & 0x7f)))
        return 0;
    return 1;
}

static unsigned long MusReadTime(const unsigned char *score, size_t score_length, size_t *position)
{
    unsigned long time;
    unsigned char value;

    time = 0;

    do
    {
        if (*position >= score_length)
            return time;

        value = score[(*position)++];
        time = (time << 7) | (value & 0x7f);
    } while (value & 0x80);

    return time;
}

static int ConvertMusToMidi(const void *data, unsigned char **midi_data, size_t *midi_length)
{
    const unsigned char *mus_data;
    unsigned short score_length;
    unsigned short score_start;
    const unsigned char *score;
    size_t position;
    midi_buffer_t midi;
    unsigned char channel_volume[16];
    int channel_map[16];
    static const unsigned char controller_map[] = {
        0x00, 0x20, 0x01, 0x07, 0x0A, 0x0B, 0x5B, 0x5D,
        0x40, 0x43, 0x78, 0x7B, 0x7E, 0x7F, 0x79
    };
    unsigned long queued_time;
    int finished;
    int i;

    mus_data = (const unsigned char *)data;
    if (memcmp(mus_data, "MUS\x1a", 4) != 0)
        return 0;

    score_length = MusReadLE16(mus_data + 4);
    score_start = MusReadLE16(mus_data + 6);
    score = mus_data + score_start;

    memset(&midi, 0, sizeof(midi));

    for (i = 0; i < 16; ++i)
    {
        channel_volume[i] = 127;
        channel_map[i] = -1;
    }

    queued_time = 0;
    position = 0;
    finished = 0;

    if (!MidiAppendData(&midi, "MThd", 4)
        || !MidiAppendBE32(&midi, 6)
        || !MidiAppendBE16(&midi, 0)
        || !MidiAppendBE16(&midi, 1)
        || !MidiAppendBE16(&midi, MIDI_TICKS_PER_QUARTER)
        || !MidiAppendData(&midi, "MTrk", 4)
        || !MidiAppendBE32(&midi, 0))
        goto fail;

    while (!finished && position < score_length)
    {
        for (;;)
        {
            unsigned char event_descriptor;
            int event_type;
            int mus_channel;
            int midi_channel;
            unsigned char key;
            unsigned char controller;
            unsigned char value;

            event_descriptor = score[position++];
            event_type = event_descriptor & 0x70;
            mus_channel = event_descriptor & 0x0f;

            if (mus_channel == 15)
            {
                midi_channel = 9;
            }
            else
            {
                if (channel_map[mus_channel] == -1)
                {
                    channel_map[mus_channel] = MidiAllocateChannel(channel_map);
                    if (!MidiWriteControllerValueless(&midi,
                                                      &queued_time,
                                                      (unsigned char)channel_map[mus_channel],
                                                      0x7b))
                        goto fail;
                }

                midi_channel = channel_map[mus_channel];
            }

            switch (event_type)
            {
            case 0x00:
                key = score[position++];
                if (!MidiWriteReleaseKey(&midi, &queued_time, (unsigned char)midi_channel, key))
                    goto fail;
                break;

            case 0x10:
                key = score[position++];
                if (key & 0x80)
                    channel_volume[mus_channel] = score[position++] & 0x7f;

                if (!MidiWritePressKey(&midi,
                                       &queued_time,
                                       (unsigned char)midi_channel,
                                       (unsigned char)(key & 0x7f),
                                       channel_volume[mus_channel]))
                    goto fail;
                break;

            case 0x20:
                value = score[position++];
                if (!MidiWritePitchWheel(&midi,
                                         &queued_time,
                                         (unsigned char)midi_channel,
                                         (unsigned short)(value * 64)))
                    goto fail;
                break;

            case 0x30:
                controller = score[position++] & 0x7f;
                if (controller < 10 || controller > 14)
                    goto fail;

                if (!MidiWriteControllerValueless(&midi,
                                                  &queued_time,
                                                  (unsigned char)midi_channel,
                                                  controller_map[controller]))
                    goto fail;
                break;

            case 0x40:
                controller = score[position++] & 0x7f;
                value = score[position++] & 0x7f;

                if (controller == 0)
                {
                    if (!MidiWritePatchChange(&midi,
                                              &queued_time,
                                              (unsigned char)midi_channel,
                                              value))
                        goto fail;
                }
                else
                {
                    if (controller < 1 || controller > 9)
                        goto fail;

                    if (!MidiWriteControllerValued(&midi,
                                                   &queued_time,
                                                   (unsigned char)midi_channel,
                                                   controller_map[controller],
                                                   value))
                        goto fail;
                }
                break;

            case 0x60:
                finished = 1;
                break;

            default:
                goto fail;
            }

            if (event_descriptor & 0x80)
                break;

            if (finished)
                break;
        }

        if (!finished)
            queued_time += MusReadTime(score, score_length, &position);
    }

    if (!MidiWriteQueuedTime(&midi, &queued_time)
        || !MidiAppendByte(&midi, 0xff)
        || !MidiAppendByte(&midi, 0x2f)
        || !MidiAppendByte(&midi, 0x00))
        goto fail;

    midi.data[18] = (unsigned char)(((midi.length - 22) >> 24) & 0xff);
    midi.data[19] = (unsigned char)(((midi.length - 22) >> 16) & 0xff);
    midi.data[20] = (unsigned char)(((midi.length - 22) >> 8) & 0xff);
    midi.data[21] = (unsigned char)((midi.length - 22) & 0xff);

    *midi_data = midi.data;
    *midi_length = midi.length;
    return 1;

fail:
    free(midi.data);
    return 0;
}

static int WriteMidiFile(const char *path, const unsigned char *midi_data, size_t midi_length)
{
    FILE *file;

    file = fopen(path, "wb");
    if (!file)
        return 0;

    if (fwrite(midi_data, 1, midi_length, file) != midi_length)
    {
        fclose(file);
        DeleteFileA(path);
        return 0;
    }

    fclose(file);
    return 1;
}

static unsigned long ReadMidiBE32(const unsigned char *data)
{
    return ((unsigned long)data[0] << 24)
         | ((unsigned long)data[1] << 16)
         | ((unsigned long)data[2] << 8)
         | (unsigned long)data[3];
}

static unsigned short ReadMidiBE16(const unsigned char *data)
{
    return (unsigned short)(((unsigned short)data[0] << 8) | (unsigned short)data[1]);
}

static int ReadMidiVarLen(const unsigned char *data, size_t length, size_t *position, unsigned long *value)
{
    int i;
    unsigned char b;

    *value = 0;

    for (i = 0; i < 4; ++i)
    {
        if (*position >= length)
            return 0;

        b = data[(*position)++];
        *value = (*value << 7) | (b & 0x7f);

        if ((b & 0x80) == 0)
            return 1;
    }

    return 0;
}

static void MusicSilenceAllChannels(void)
{
    int channel;

    for (channel = 0; channel < 16; ++channel)
    {
        MusicSendShortMessage((unsigned char)(0xB0 | channel), 0x7B, 0x00, 1);
        MusicSendShortMessage((unsigned char)(0xB0 | channel), 0x78, 0x00, 1);
        MusicSendShortMessage((unsigned char)(0xB0 | channel), 0x79, 0x00, 1);
    }
}

static int MusicWaitUntil(ULONGLONG *target_time_ms)
{
    for (;;)
    {
        ULONGLONG now;
        DWORD remaining;
        DWORD slice;
        DWORD result;

        if (music_shutdown_request || music_stop_request)
            return 0;

        while (music_paused)
        {
            result = WaitForSingleObject(music_wake_event, 10);
            if (music_shutdown_request || music_stop_request)
                return 0;
            if (result == WAIT_FAILED)
                return 0;
            *target_time_ms = GetTickCount64();
        }

        now = GetTickCount64();
        if (now >= *target_time_ms)
            return 1;

        remaining = (DWORD)(*target_time_ms - now);
        slice = remaining > 2 ? remaining - 1 : remaining;
        if (slice == 0)
            slice = 1;

        result = WaitForSingleObject(music_wake_event, slice);
        if (result == WAIT_FAILED)
            return 0;
    }
}

static int MusicSendShortMessage(unsigned char status,
                                 unsigned char data1,
                                 unsigned char data2,
                                 int has_two_data_bytes)
{
    DWORD message;

    if (!music_out_device)
        return 0;

    message = status | ((DWORD)data1 << 8);
    if (has_two_data_bytes)
        message |= ((DWORD)data2 << 16);

    return midiOutShortMsg(music_out_device, message) == MMSYSERR_NOERROR;
}

static DWORD WINAPI MusicThreadProc(LPVOID unused)
{
    unused = unused;

    while (!music_shutdown_request)
    {
        const unsigned char *track;
        size_t track_length;
        size_t position;
        unsigned short division;
        unsigned long tempo;
        unsigned char running_status;
        ULONGLONG target_time_ms;
        music_song_t *song;

        if (!music_current_handle || !music_songs[music_current_handle - 1].used)
        {
            WaitForSingleObject(music_wake_event, 50);
            continue;
        }

        song = &music_songs[music_current_handle - 1];
        if (song->length < 22 || memcmp(song->data, "MThd", 4) != 0 || memcmp(song->data + 14, "MTrk", 4) != 0)
        {
            music_current_handle = 0;
            continue;
        }

        division = ReadMidiBE16(song->data + 12);
        track_length = ReadMidiBE32(song->data + 18);
        if (22 + track_length > song->length || division == 0)
        {
            music_current_handle = 0;
            continue;
        }

        track = song->data + 22;
        position = 0;
        tempo = 500000UL;
        running_status = 0;
        target_time_ms = GetTickCount64();

        while (position < track_length && !music_shutdown_request && !music_stop_request)
        {
            unsigned long delta;
            unsigned long long wait_us;
            unsigned char status;

            if (!ReadMidiVarLen(track, track_length, &position, &delta))
                break;

            wait_us = ((unsigned long long)delta * (unsigned long long)tempo) / division;
            target_time_ms += (ULONGLONG)((wait_us + 999ULL) / 1000ULL);
            if (!MusicWaitUntil(&target_time_ms))
                break;

            if (position >= track_length)
                break;

            status = track[position++];
            if ((status & 0x80) == 0)
            {
                if (!running_status)
                    break;
                position--;
                status = running_status;
            }
            else
            {
                running_status = status;
            }

            switch (status & 0xf0)
            {
            case 0x80:
            case 0x90:
            case 0xA0:
            case 0xB0:
            case 0xE0:
            {
                unsigned char data1;
                unsigned char data2;

                if (position + 1 >= track_length)
                    position = track_length;
                else
                {
                    data1 = track[position++];
                    data2 = track[position++];
                    MusicSendShortMessage(status, data1, data2, 1);
                }
                break;
            }

            case 0xC0:
            case 0xD0:
                if (position >= track_length)
                    position = track_length;
                else
                    MusicSendShortMessage(status, track[position++], 0, 0);
                break;

            default:
                if (status == 0xFF)
                {
                    unsigned char meta_type;
                    unsigned long meta_length;

                    if (position >= track_length)
                        break;

                    meta_type = track[position++];
                    if (!ReadMidiVarLen(track, track_length, &position, &meta_length))
                        break;
                    if (position + meta_length > track_length)
                        break;

                    if (meta_type == 0x51 && meta_length == 3)
                    {
                        tempo = ((unsigned long)track[position] << 16)
                              | ((unsigned long)track[position + 1] << 8)
                              | (unsigned long)track[position + 2];
                    }
                    else if (meta_type == 0x2F)
                    {
                        position = track_length;
                    }

                    position += meta_length;
                }
                else if (status == 0xF0 || status == 0xF7)
                {
                    unsigned long sysex_length;

                    if (!ReadMidiVarLen(track, track_length, &position, &sysex_length))
                        break;
                    if (position + sysex_length > track_length)
                        break;
                    position += sysex_length;
                }
                else
                {
                    position = track_length;
                }
                break;
            }
        }

        if (music_shutdown_request)
            break;

        if (music_stop_request)
        {
            MusicSilenceAllChannels();
            music_stop_request = 0;
            continue;
        }

        MusicSilenceAllChannels();

        if (!music_looping)
            music_current_handle = 0;
    }

    music_thread_running = 0;
    return 0;
}

static unsigned char *CreateWaveData(const unsigned char *samples, int sample_count, DWORD *wave_size)
{
    unsigned char *wave;
    DWORD riff_size;
    DWORD data_size;

    data_size = (DWORD)sample_count;
    *wave_size = 44 + data_size;
    wave = (unsigned char *)malloc(*wave_size);
    if (!wave)
        return NULL;

    riff_size = *wave_size - 8;

    memcpy(wave + 0, "RIFF", 4);
    memcpy(wave + 4, &riff_size, 4);
    memcpy(wave + 8, "WAVEfmt ", 8);

    {
        DWORD fmt_size = 16;
        WORD format = 1;
        WORD channels_count = 1;
        DWORD sample_rate = SAMPLERATE;
        DWORD byte_rate = SAMPLERATE;
        WORD block_align = 1;
        WORD bits_per_sample = 8;

        memcpy(wave + 16, &fmt_size, 4);
        memcpy(wave + 20, &format, 2);
        memcpy(wave + 22, &channels_count, 2);
        memcpy(wave + 24, &sample_rate, 4);
        memcpy(wave + 28, &byte_rate, 4);
        memcpy(wave + 32, &block_align, 2);
        memcpy(wave + 34, &bits_per_sample, 2);
    }

    memcpy(wave + 36, "data", 4);
    memcpy(wave + 40, &data_size, 4);
    memcpy(wave + 44, samples, data_size);
    return wave;
}

static void MusicApplyVolume(void)
{
    int channel;
    int volume;

    if (!music_out_device)
        return;

    volume = MixerVolumeFromDoom(snd_MusicVolume);

    for (channel = 0; channel < 16; ++channel)
    {
        MusicSendShortMessage((unsigned char)(0xB0 | channel), 0x07,
                              (unsigned char)volume, 1);
        MusicSendShortMessage((unsigned char)(0xB0 | channel), 0x0B,
                              (unsigned char)volume, 1);
    }
}

static void MusicStopPlayback(void)
{
    music_stop_request = 1;
    if (music_wake_event)
        SetEvent(music_wake_event);
    MusicSilenceAllChannels();
    music_current_handle = 0;
    music_looping = 0;
    music_paused = 0;
}

static void SoundReclaimBuffers(void)
{
    int i;

    if (!sound_initialized)
        return;

    for (i = 0; i < AUDIO_BUFFER_COUNT; ++i)
    {
        if (!sound_buffer_ready[i] && (sound_headers[i].dwFlags & WHDR_DONE))
            sound_buffer_ready[i] = 1;
    }
}

static int SoundHasReadyBuffer(void)
{
    int i;

    for (i = 0; i < AUDIO_BUFFER_COUNT; ++i)
        if (sound_buffer_ready[i])
            return 1;

    return 0;
}

static DWORD WINAPI SoundThreadProc(LPVOID unused)
{
    unused = unused;

    while (sound_thread_running)
    {
        HANDLE wait_handles[2];
        DWORD wait_result;

        wait_handles[0] = sound_shutdown_event;
        wait_handles[1] = sound_wake_event;

        wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, 2);
        if (wait_result == WAIT_OBJECT_0)
            break;

        EnterCriticalSection(&sound_lock);
        SoundReclaimBuffers();
        while (SoundHasReadyBuffer())
        {
            I_UpdateSound();
            I_SubmitSound();
        }
        LeaveCriticalSection(&sound_lock);
    }

    return 0;
}

static void CALLBACK SoundWaveOutProc(HWAVEOUT hwo,
                                      UINT uMsg,
                                      DWORD_PTR dwInstance,
                                      DWORD_PTR dwParam1,
                                      DWORD_PTR dwParam2)
{
    int i;

    hwo = hwo;
    dwInstance = dwInstance;
    dwParam2 = dwParam2;

    if (uMsg != WOM_DONE)
        return;

    for (i = 0; i < AUDIO_BUFFER_COUNT; ++i)
    {
        if ((WAVEHDR *)dwParam1 == &sound_headers[i])
        {
            sound_buffer_ready[i] = 1;
            if (sound_wake_event)
                SetEvent(sound_wake_event);
            break;
        }
    }
}

static void SoundSetChannelParams(int slot, int volume, int separation, int step)
{
    int rightvol;
    int leftvol;

    volume = MixerVolumeFromDoom(volume);

    separation += 1;
    leftvol = volume - ((volume * separation * separation) >> 16);
    separation = separation - 257;
    rightvol = volume - ((volume * separation * separation) >> 16);

    if (rightvol < 0)
        rightvol = 0;
    else if (rightvol > 127)
        rightvol = 127;

    if (leftvol < 0)
        leftvol = 0;
    else if (leftvol > 127)
        leftvol = 127;

    channelleftvol_lookup[slot] = &vol_lookup[leftvol * 256];
    channelrightvol_lookup[slot] = &vol_lookup[rightvol * 256];
    channelstep[slot] = step;
}

static void *getsfx(char *sfxname, int *len)
{
    unsigned char *sfx;
    unsigned char *paddedsfx;
    int i;
    int size;
    int paddedsize;
    char name[20];
    int sfxlump;

    sprintf(name, "ds%s", sfxname);

    if (W_CheckNumForName(name) == -1)
        sfxlump = W_GetNumForName("dspistol");
    else
        sfxlump = W_GetNumForName(name);

    size = W_LumpLength(sfxlump);
    sfx = (unsigned char *)W_CacheLumpNum(sfxlump, PU_STATIC);

    paddedsize = ((size - 8 + (SAMPLECOUNT - 1)) / SAMPLECOUNT) * SAMPLECOUNT;
    paddedsfx = (unsigned char *)malloc(paddedsize + 8);
    if (!paddedsfx)
        I_Error("getsfx: failed to allocate %d bytes", paddedsize + 8);

    memcpy(paddedsfx, sfx, size);
    for (i = size; i < paddedsize + 8; ++i)
        paddedsfx[i] = 128;

    Z_Free(sfx);
    *len = paddedsize;
    return (void *)(paddedsfx + 8);
}

static int addsfx(int sfxid, int volume, int step, int separation)
{
    static unsigned short handlenums = 0;
    int i;
    int oldest;
    int oldestnum;
    int slot;
    if (sfxid == sfx_sawup
        || sfxid == sfx_sawidl
        || sfxid == sfx_sawful
        || sfxid == sfx_sawhit
        || sfxid == sfx_stnmov
        || sfxid == sfx_pistol)
    {
        for (i = 0; i < NUM_CHANNELS; ++i)
        {
            if (channels[i] && channelids[i] == sfxid)
            {
                channels[i] = NULL;
                channelhandles[i] = 0;
                break;
            }
        }
    }

    oldest = gametic;
    oldestnum = 0;

    for (i = 0; (i < NUM_CHANNELS) && channels[i]; ++i)
    {
        if (channelstart[i] < oldest)
        {
            oldestnum = i;
            oldest = channelstart[i];
        }
    }

    slot = (i == NUM_CHANNELS) ? oldestnum : i;

    channels[slot] = (unsigned char *)S_sfx[sfxid].data;
    channelsend[slot] = channels[slot] + lengths[sfxid];

    if (!handlenums)
        handlenums = 100;

    channelhandles[slot] = handlenums++;
    channelstepremainder[slot] = 0;
    channelstart[slot] = gametic;
    SoundSetChannelParams(slot, volume, separation, step);
    channelids[slot] = sfxid;

    return channelhandles[slot];
}

void I_SetChannels(void)
{
    int i;
    int j;
    int *steptablemid;

    steptablemid = steptable + 128;

    for (i = 0; i < NUM_CHANNELS; ++i)
    {
        channels[i] = NULL;
        channelsend[i] = NULL;
        channelstep[i] = 0;
        channelstepremainder[i] = 0;
        channelstart[i] = 0;
        channelhandles[i] = 0;
        channelids[i] = 0;
        channelleftvol_lookup[i] = vol_lookup;
        channelrightvol_lookup[i] = vol_lookup;
    }

    for (i = -128; i < 128; ++i)
        steptablemid[i] = (int)(pow(2.0, (i / 64.0)) * 65536.0);

    for (i = 0; i < 128; ++i)
        for (j = 0; j < 256; ++j)
            vol_lookup[i * 256 + j] = (i * (j - 128) * 256) / 127;
}

void I_SetSfxVolume(int volume)
{
    snd_SfxVolume = volume;
}

void I_SetMusicVolume(int volume)
{
    snd_MusicVolume = volume;
    MusicApplyVolume();
}

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];

    sprintf(namebuf, "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

void I_StopSound(int handle)
{
    int i;

    if (sound_lock_initialized)
        EnterCriticalSection(&sound_lock);

    for (i = 0; i < NUM_CHANNELS; ++i)
    {
        if (channelhandles[i] == handle)
        {
            channels[i] = NULL;
            channelsend[i] = NULL;
            channelhandles[i] = 0;
            channelids[i] = 0;
            break;
        }
    }

    if (sound_lock_initialized)
        LeaveCriticalSection(&sound_lock);

    if (sound_wake_event)
        SetEvent(sound_wake_event);
}

int I_SoundIsPlaying(int handle)
{
    int i;

    for (i = 0; i < NUM_CHANNELS; ++i)
        if (channelhandles[i] == handle && channels[i])
            return 1;

    return 0;
}

void I_UpdateSound(void)
{
    if (sound_thread_running && GetCurrentThreadId() != sound_thread_id)
        return;

    register unsigned int sample;
    register int dl;
    register int dr;
    signed short *leftout;
    signed short *rightout;
    signed short *leftend;
    int step;
    int chan;

    leftout = mixbuffer;
    rightout = mixbuffer + 1;
    step = 2;
    leftend = mixbuffer + SAMPLECOUNT * step;

    while (leftout != leftend)
    {
        dl = 0;
        dr = 0;

        for (chan = 0; chan < NUM_CHANNELS; ++chan)
        {
            if (channels[chan])
            {
                sample = *channels[chan];
                dl += channelleftvol_lookup[chan][sample];
                dr += channelrightvol_lookup[chan][sample];
                channelstepremainder[chan] += channelstep[chan];
                channels[chan] += channelstepremainder[chan] >> 16;
                channelstepremainder[chan] &= 65536 - 1;

                if (channels[chan] >= channelsend[chan])
                {
                    channels[chan] = NULL;
                    channelsend[chan] = NULL;
                    channelhandles[chan] = 0;
                    channelids[chan] = 0;
                }
            }
        }

        if (dl > 0x7fff)
            *leftout = 0x7fff;
        else if (dl < -0x8000)
            *leftout = -0x8000;
        else
            *leftout = (signed short)dl;

        if (dr > 0x7fff)
            *rightout = 0x7fff;
        else if (dr < -0x8000)
            *rightout = -0x8000;
        else
            *rightout = (signed short)dr;

        leftout += step;
        rightout += step;
    }
}

void I_SubmitSound(void)
{
    if (sound_thread_running && GetCurrentThreadId() != sound_thread_id)
        return;

    int i;
    MMRESULT result;

    if (!sound_initialized)
        return;

    for (i = 0; i < AUDIO_BUFFER_COUNT; ++i)
    {
        if (sound_buffer_ready[i])
        {
            memcpy(sound_buffers[i], mixbuffer, MIXBUFFER_BYTES);
            sound_headers[i].dwBufferLength = MIXBUFFER_BYTES;
            sound_buffer_ready[i] = 0;
            result = waveOutWrite(sound_device, &sound_headers[i], sizeof(WAVEHDR));
            if (result != MMSYSERR_NOERROR)
                sound_buffer_ready[i] = 1;
            return;
        }
    }
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
    int i;

    if (sound_lock_initialized)
        EnterCriticalSection(&sound_lock);

    for (i = 0; i < NUM_CHANNELS; ++i)
    {
        if (channelhandles[i] == handle && channels[i])
        {
            SoundSetChannelParams(i, vol, sep, steptable[pitch]);
            break;
        }
    }

    if (sound_lock_initialized)
        LeaveCriticalSection(&sound_lock);
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    int handle;

    priority = 0;

    if (id <= 0 || id >= NUMSFX || !S_sfx[id].data)
        return 0;

    if (sound_lock_initialized)
        EnterCriticalSection(&sound_lock);

    handle = addsfx(id, vol, steptable[pitch], sep);

    if (sound_lock_initialized)
        LeaveCriticalSection(&sound_lock);

    if (sound_wake_event)
        SetEvent(sound_wake_event);

    return handle;
}

void I_ShutdownSound(void)
{
    int i;

    for (i = 0; i < NUMSFX; ++i)
    {
        if (sfx_wave_data[i] && (!S_sfx[i].link || S_sfx[i].link->data != S_sfx[i].data))
        {
            free(sfx_wave_data[i]);
            sfx_wave_data[i] = NULL;
            sfx_wave_size[i] = 0;
        }
    }

    sound_thread_running = 0;
    if (sound_shutdown_event)
        SetEvent(sound_shutdown_event);
    if (sound_thread)
    {
        WaitForSingleObject(sound_thread, 1000);
        CloseHandle(sound_thread);
        sound_thread = NULL;
    }
    if (sound_shutdown_event)
    {
        CloseHandle(sound_shutdown_event);
        sound_shutdown_event = NULL;
    }
    if (sound_wake_event)
    {
        CloseHandle(sound_wake_event);
        sound_wake_event = NULL;
    }

    if (!sound_device)
        goto cleanup;

    waveOutReset(sound_device);

    for (i = 0; i < AUDIO_BUFFER_COUNT; ++i)
        waveOutUnprepareHeader(sound_device, &sound_headers[i], sizeof(WAVEHDR));

    waveOutClose(sound_device);
    sound_device = NULL;
    sound_initialized = 0;

cleanup:
    if (sound_lock_initialized)
    {
        DeleteCriticalSection(&sound_lock);
        sound_lock_initialized = 0;
    }

    sound_thread_id = 0;
}

void I_InitSound(void)
{
    WAVEFORMATEX wave_format;
    MMRESULT result;
    int i;

    memset(&wave_format, 0, sizeof(wave_format));
    wave_format.wFormatTag = WAVE_FORMAT_PCM;
    wave_format.nChannels = 2;
    wave_format.nSamplesPerSec = SAMPLERATE;
    wave_format.wBitsPerSample = 16;
    wave_format.nBlockAlign = (WORD)(wave_format.nChannels * (wave_format.wBitsPerSample / 8));
    wave_format.nAvgBytesPerSec = wave_format.nSamplesPerSec * wave_format.nBlockAlign;

    result = waveOutOpen(&sound_device,
                         WAVE_MAPPER,
                         &wave_format,
                         (DWORD_PTR)SoundWaveOutProc,
                         0,
                         CALLBACK_FUNCTION);
    if (result == MMSYSERR_NOERROR)
    {
        InitializeCriticalSection(&sound_lock);
        sound_lock_initialized = 1;
        I_SetChannels();
        memset(sound_headers, 0, sizeof(sound_headers));
        for (i = 0; i < AUDIO_BUFFER_COUNT; ++i)
        {
            sound_headers[i].lpData = (LPSTR)sound_buffers[i];
            sound_headers[i].dwBufferLength = MIXBUFFER_BYTES;
            result = waveOutPrepareHeader(sound_device, &sound_headers[i], sizeof(WAVEHDR));
            if (result != MMSYSERR_NOERROR)
            {
                I_ShutdownSound();
                break;
            }
            sound_buffer_ready[i] = 1;
        }
        sound_shutdown_event = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (!sound_shutdown_event)
        {
            I_ShutdownSound();
            return;
        }
        sound_wake_event = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!sound_wake_event)
        {
            I_ShutdownSound();
            return;
        }

        sound_thread_running = 1;
        sound_thread = CreateThread(NULL, 0, SoundThreadProc, NULL, 0, NULL);
        if (!sound_thread)
        {
            I_ShutdownSound();
            return;
        }
        sound_thread_id = GetThreadId(sound_thread);

        sound_initialized = 1;
        SetEvent(sound_wake_event);
    }

    memset(sfx_wave_data, 0, sizeof(sfx_wave_data));
    memset(sfx_wave_size, 0, sizeof(sfx_wave_size));

    for (i = 1; i < NUMSFX; ++i)
    {
        if (!S_sfx[i].link)
        {
            unsigned char *raw;

            raw = getsfx(S_sfx[i].name, &lengths[i]);
            S_sfx[i].data = raw;
            sfx_wave_data[i] = CreateWaveData(raw, lengths[i], &sfx_wave_size[i]);
        }
        else
        {
            ptrdiff_t link_index;

            S_sfx[i].data = S_sfx[i].link->data;
            link_index = S_sfx[i].link - S_sfx;
            lengths[i] = lengths[link_index];
            sfx_wave_data[i] = sfx_wave_data[link_index];
            sfx_wave_size[i] = sfx_wave_size[link_index];
        }
    }

    memset(mixbuffer, 0, sizeof(mixbuffer));
}

void I_InitMusic(void)
{
    MMRESULT result;

    result = midiOutOpen(&music_out_device, MIDI_MAPPER, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR)
        return;

    timeBeginPeriod(1);

    music_wake_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!music_wake_event)
    {
        timeEndPeriod(1);
        midiOutClose(music_out_device);
        music_out_device = NULL;
        return;
    }

    music_stop_request = 0;
    music_shutdown_request = 0;
    music_thread_running = 1;
    music_thread = CreateThread(NULL, 0, MusicThreadProc, NULL, 0, NULL);
    if (!music_thread)
    {
        CloseHandle(music_wake_event);
        music_wake_event = NULL;
        timeEndPeriod(1);
        midiOutClose(music_out_device);
        music_out_device = NULL;
        music_thread_running = 0;
        return;
    }

    music_initialized = 1;
}

void I_ShutdownMusic(void)
{
    int i;

    if (!music_initialized)
        return;

    MusicStopPlayback();
    music_shutdown_request = 1;
    if (music_wake_event)
        SetEvent(music_wake_event);

    if (music_thread)
    {
        WaitForSingleObject(music_thread, 1000);
        CloseHandle(music_thread);
        music_thread = NULL;
    }

    if (music_wake_event)
    {
        CloseHandle(music_wake_event);
        music_wake_event = NULL;
    }

    if (music_out_device)
    {
        MusicSilenceAllChannels();
        midiOutReset(music_out_device);
        midiOutClose(music_out_device);
        music_out_device = NULL;
        timeEndPeriod(1);
    }

    for (i = 0; i < MUSIC_SONG_LIMIT; ++i)
    {
        if (music_songs[i].used)
        {
            free(music_songs[i].data);
            music_songs[i].used = 0;
            music_songs[i].data = NULL;
            music_songs[i].length = 0;
        }
    }

    music_initialized = 0;
}

void I_PlaySong(int handle, int looping)
{
    if (handle < 1 || handle > MUSIC_SONG_LIMIT || !music_songs[handle - 1].used)
        return;

    music_stop_request = 0;
    music_current_handle = handle;
    music_looping = looping;
    music_paused = 0;
    if (music_wake_event)
        SetEvent(music_wake_event);
}

void I_PauseSong(int handle)
{
    if (handle != music_current_handle)
        return;

    music_paused = 1;
    if (music_wake_event)
        SetEvent(music_wake_event);
}

void I_ResumeSong(int handle)
{
    if (handle != music_current_handle)
        return;

    music_paused = 0;
    if (music_wake_event)
        SetEvent(music_wake_event);
}

void I_StopSong(int handle)
{
    if (handle == music_current_handle)
        MusicStopPlayback();
}

void I_UnRegisterSong(int handle)
{
    music_song_t *song;

    if (handle < 1 || handle > MUSIC_SONG_LIMIT)
        return;

    if (handle == music_current_handle)
        MusicStopPlayback();

    song = &music_songs[handle - 1];
    if (song->used)
    {
        free(song->data);
        song->used = 0;
        song->data = NULL;
        song->length = 0;
    }
}

int I_RegisterSong(void *data)
{
    unsigned char *midi_data;
    size_t midi_length;
    int i;

    midi_data = NULL;
    midi_length = 0;

    if (!ConvertMusToMidi(data, &midi_data, &midi_length))
        return 0;

    for (i = 0; i < MUSIC_SONG_LIMIT; ++i)
    {
        if (!music_songs[i].used)
        {
            music_songs[i].used = 1;
            music_songs[i].data = midi_data;
            music_songs[i].length = midi_length;
            return i + 1;
        }
    }

    free(midi_data);
    return 0;
}

int I_QrySongPlaying(int handle)
{
    if (handle != music_current_handle)
        return 0;

    return music_thread_running && !music_stop_request;
}
