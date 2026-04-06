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

FILE *sndserver = 0;
char *sndserver_filename = "";

#define SAMPLECOUNT 512
#define NUM_CHANNELS 8
#define BUFMUL 4
#define MIXBUFFER_BYTES (SAMPLECOUNT * BUFMUL)
#define SAMPLERATE 11025
#define AUDIO_BUFFER_COUNT 4
#define MIDI_TICKS_PER_QUARTER 140
#define MIDI_TEMPO_USEC 1000000UL
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

typedef struct
{
    int used;
    char path[MAX_PATH];
} music_song_t;

static music_song_t music_songs[MUSIC_SONG_LIMIT];
static HMIDIOUT music_volume_device;
static int music_device_open;
static int music_initialized;
static int music_current_handle;
static int music_looping;
static int music_paused;

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
    if (mus_channel == 15)
        return 9;

    if (mus_channel >= 9)
        return mus_channel + 1;

    return mus_channel;
}

static int MidiControllerForMus(int controller)
{
    static const int controller_map[] = { -1, 0, 1, 7, 10, 11, 91, 93, 64, 67 };

    if (controller < 1 || controller > 9)
        return -1;

    return controller_map[controller];
}

static int MidiSystemControllerForMus(int controller)
{
    switch (controller)
    {
    case 10:
        return 120;
    case 11:
        return 123;
    case 12:
        return 126;
    case 13:
        return 127;
    case 14:
        return 121;
    default:
        return -1;
    }
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
    midi_buffer_t track;
    midi_buffer_t midi;
    unsigned char channel_volume[16];
    unsigned long pending_delta;
    int finished;
    int status;
    int i;

    mus_data = (const unsigned char *)data;
    if (memcmp(mus_data, "MUS\x1a", 4) != 0)
        return 0;

    score_length = MusReadLE16(mus_data + 4);
    score_start = MusReadLE16(mus_data + 6);
    score = mus_data + score_start;

    memset(&track, 0, sizeof(track));
    memset(&midi, 0, sizeof(midi));

    for (i = 0; i < 16; ++i)
        channel_volume[i] = 64;

    pending_delta = 0;
    position = 0;
    finished = 0;

    if (!MidiAppendVarLen(&track, 0)
        || !MidiAppendByte(&track, 0xff)
        || !MidiAppendByte(&track, 0x51)
        || !MidiAppendByte(&track, 0x03)
        || !MidiAppendByte(&track, (unsigned char)((MIDI_TEMPO_USEC >> 16) & 0xff))
        || !MidiAppendByte(&track, (unsigned char)((MIDI_TEMPO_USEC >> 8) & 0xff))
        || !MidiAppendByte(&track, (unsigned char)(MIDI_TEMPO_USEC & 0xff)))
    {
        free(track.data);
        return 0;
    }

    while (!finished && position < score_length)
    {
        unsigned long group_delta;
        int first_event;

        group_delta = pending_delta;
        pending_delta = 0;
        first_event = 1;

        for (;;)
        {
            unsigned char event;
            int event_type;
            int mus_channel;
            int midi_channel;
            unsigned long delta;

            event = score[position++];
            event_type = (event >> 4) & 0x07;
            mus_channel = event & 0x0f;
            midi_channel = MidiChannelForMus(mus_channel);
            delta = first_event ? group_delta : 0;
            first_event = 0;

            switch (event_type)
            {
            case 0:
            {
                unsigned char note;

                note = score[position++] & 0x7f;
                status = MidiAppendShortEvent(&track,
                                              delta,
                                              (unsigned char)(0x80 | midi_channel),
                                              note,
                                              0);
                if (!status)
                    goto fail;
                break;
            }

            case 1:
            {
                unsigned char note;
                unsigned char velocity;

                note = score[position++];
                velocity = channel_volume[mus_channel];

                if (note & 0x80)
                {
                    note &= 0x7f;
                    velocity = score[position++] & 0x7f;
                    channel_volume[mus_channel] = velocity;
                }

                status = MidiAppendShortEvent(&track,
                                              delta,
                                              (unsigned char)(0x90 | midi_channel),
                                              note,
                                              velocity);
                if (!status)
                    goto fail;
                break;
            }

            case 2:
            {
                unsigned char value;
                unsigned short bend;

                value = score[position++];
                bend = (unsigned short)(value << 6);
                status = MidiAppendShortEvent(&track,
                                              delta,
                                              (unsigned char)(0xe0 | midi_channel),
                                              (unsigned char)(bend & 0x7f),
                                              (unsigned char)((bend >> 7) & 0x7f));
                if (!status)
                    goto fail;
                break;
            }

            case 3:
            {
                int controller;
                int midi_controller;

                controller = score[position++] & 0x7f;
                midi_controller = MidiSystemControllerForMus(controller);
                if (midi_controller < 0)
                    goto fail;

                status = MidiAppendShortEvent(&track,
                                              delta,
                                              (unsigned char)(0xb0 | midi_channel),
                                              (unsigned char)midi_controller,
                                              (unsigned char)((controller == 12) ? 1 : 0));
                if (!status)
                    goto fail;
                break;
            }

            case 4:
            {
                int controller;
                int value;
                int midi_controller;

                controller = score[position++] & 0x7f;
                value = score[position++] & 0x7f;

                if (controller == 0)
                {
                    status = MidiAppendProgramChange(&track,
                                                     delta,
                                                     (unsigned char)(0xc0 | midi_channel),
                                                     (unsigned char)value);
                }
                else
                {
                    midi_controller = MidiControllerForMus(controller);
                    if (midi_controller < 0)
                        goto fail;

                    status = MidiAppendShortEvent(&track,
                                                  delta,
                                                  (unsigned char)(0xb0 | midi_channel),
                                                  (unsigned char)midi_controller,
                                                  (unsigned char)value);
                }

                if (!status)
                    goto fail;
                break;
            }

            case 6:
                finished = 1;
                break;

            default:
                goto fail;
            }

            if (event & 0x80)
            {
                pending_delta = MusReadTime(score, score_length, &position);
                break;
            }

            if (finished)
                break;
        }
    }

    if (!MidiAppendVarLen(&track, pending_delta)
        || !MidiAppendByte(&track, 0xff)
        || !MidiAppendByte(&track, 0x2f)
        || !MidiAppendByte(&track, 0x00))
        goto fail;

    if (!MidiAppendData(&midi, "MThd", 4)
        || !MidiAppendBE32(&midi, 6)
        || !MidiAppendBE16(&midi, 0)
        || !MidiAppendBE16(&midi, 1)
        || !MidiAppendBE16(&midi, MIDI_TICKS_PER_QUARTER)
        || !MidiAppendData(&midi, "MTrk", 4)
        || !MidiAppendBE32(&midi, (unsigned long)track.length)
        || !MidiAppendData(&midi, track.data, track.length))
        goto fail;

    free(track.data);
    *midi_data = midi.data;
    *midi_length = midi.length;
    return 1;

fail:
    free(track.data);
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

static void MusicApplyVolume(void)
{
    DWORD volume;
    int scaled_volume;

    if (!music_device_open)
        return;

    scaled_volume = MixerVolumeFromDoom(snd_MusicVolume);
    volume = (DWORD)((scaled_volume * 0xffff) / 127);
    volume |= volume << 16;
    midiOutSetVolume(music_volume_device, volume);
}

static void MusicStopPlayback(void)
{
    mciSendStringA("stop " MUSIC_ALIAS, NULL, 0, NULL);
    mciSendStringA("close " MUSIC_ALIAS, NULL, 0, NULL);
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
    int rightvol;
    int leftvol;

    volume = MixerVolumeFromDoom(volume);

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
    channelstep[slot] = step;
    channelstepremainder[slot] = 0;
    channelstart[slot] = gametic;

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

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
    priority = 0;
    return addsfx(id, vol, steptable[pitch], sep);
}

void I_StopSound(int handle)
{
    int i;

    for (i = 0; i < NUM_CHANNELS; ++i)
    {
        if (channelhandles[i] == handle)
        {
            channels[i] = NULL;
            channelsend[i] = NULL;
            channelhandles[i] = 0;
            break;
        }
    }
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
    unsigned int sample;
    int dl;
    int dr;
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
    int i;
    MMRESULT result;

    if (!sound_initialized)
        return;

    SoundReclaimBuffers();

    for (i = 0; i < AUDIO_BUFFER_COUNT; ++i)
    {
        if (sound_buffer_ready[i])
        {
            memcpy(sound_buffers[i], mixbuffer, MIXBUFFER_BYTES);
            sound_headers[i].dwBufferLength = MIXBUFFER_BYTES;
            sound_headers[i].dwFlags &= ~WHDR_DONE;
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
    int separation;
    int rightvol;
    int leftvol;

    vol = MixerVolumeFromDoom(vol);

    for (i = 0; i < NUM_CHANNELS; ++i)
    {
        if (channelhandles[i] == handle && channels[i])
        {
            channelstep[i] = steptable[pitch];

            separation = sep + 1;
            leftvol = vol - ((vol * separation * separation) >> 16);
            separation = separation - 257;
            rightvol = vol - ((vol * separation * separation) >> 16);

            if (rightvol < 0)
                rightvol = 0;
            else if (rightvol > 127)
                rightvol = 127;

            if (leftvol < 0)
                leftvol = 0;
            else if (leftvol > 127)
                leftvol = 127;

            channelleftvol_lookup[i] = &vol_lookup[leftvol * 256];
            channelrightvol_lookup[i] = &vol_lookup[rightvol * 256];
            return;
        }
    }
}

void I_ShutdownSound(void)
{
    int i;

    if (!sound_initialized)
        return;

    waveOutReset(sound_device);

    for (i = 0; i < AUDIO_BUFFER_COUNT; ++i)
        waveOutUnprepareHeader(sound_device, &sound_headers[i], sizeof(WAVEHDR));

    waveOutClose(sound_device);
    sound_device = NULL;
    sound_initialized = 0;
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

    result = waveOutOpen(&sound_device, WAVE_MAPPER, &wave_format, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR)
    {
        fprintf(stderr, "I_InitSound: failed to open waveOut device\n");
        return;
    }

    memset(sound_headers, 0, sizeof(sound_headers));
    for (i = 0; i < AUDIO_BUFFER_COUNT; ++i)
    {
        sound_headers[i].lpData = (LPSTR)sound_buffers[i];
        sound_headers[i].dwBufferLength = MIXBUFFER_BYTES;
        result = waveOutPrepareHeader(sound_device, &sound_headers[i], sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR)
        {
            fprintf(stderr, "I_InitSound: failed to prepare audio buffer\n");
            I_ShutdownSound();
            return;
        }
        sound_buffer_ready[i] = 1;
    }

    for (i = 1; i < NUMSFX; ++i)
    {
        if (!S_sfx[i].link)
        {
            S_sfx[i].data = getsfx(S_sfx[i].name, &lengths[i]);
        }
        else
        {
            ptrdiff_t link_index;

            S_sfx[i].data = S_sfx[i].link->data;
            link_index = S_sfx[i].link - S_sfx;
            lengths[i] = lengths[link_index];
        }
    }

    memset(mixbuffer, 0, sizeof(mixbuffer));
    sound_initialized = 1;
}

void I_InitMusic(void)
{
    MMRESULT result;

    result = midiOutOpen(&music_volume_device, MIDI_MAPPER, 0, 0, CALLBACK_NULL);
    if (result == MMSYSERR_NOERROR)
    {
        music_device_open = 1;
        MusicApplyVolume();
    }

    music_initialized = 1;
}

void I_ShutdownMusic(void)
{
    int i;

    if (!music_initialized)
        return;

    MusicStopPlayback();

    if (music_device_open)
    {
        midiOutClose(music_volume_device);
        music_device_open = 0;
        music_volume_device = NULL;
    }

    for (i = 0; i < MUSIC_SONG_LIMIT; ++i)
    {
        if (music_songs[i].used)
        {
            DeleteFileA(music_songs[i].path);
            music_songs[i].used = 0;
            music_songs[i].path[0] = '\0';
        }
    }

    music_initialized = 0;
}

void I_PlaySong(int handle, int looping)
{
    char command[2 * MAX_PATH + 64];
    music_song_t *song;

    if (handle < 1 || handle > MUSIC_SONG_LIMIT || !music_songs[handle - 1].used)
        return;

    MusicStopPlayback();

    song = &music_songs[handle - 1];
    sprintf(command, "open \"%s\" type sequencer alias %s", song->path, MUSIC_ALIAS);
    if (mciSendStringA(command, NULL, 0, NULL) != 0)
        return;

    if (looping)
        sprintf(command, "play %s from 0 repeat", MUSIC_ALIAS);
    else
        sprintf(command, "play %s from 0", MUSIC_ALIAS);

    if (mciSendStringA(command, NULL, 0, NULL) != 0)
    {
        MusicStopPlayback();
        return;
    }

    music_current_handle = handle;
    music_looping = looping;
    music_paused = 0;
    MusicApplyVolume();
}

void I_PauseSong(int handle)
{
    if (handle != music_current_handle)
        return;

    if (mciSendStringA("pause " MUSIC_ALIAS, NULL, 0, NULL) == 0)
        music_paused = 1;
}

void I_ResumeSong(int handle)
{
    if (handle != music_current_handle)
        return;

    if (mciSendStringA("resume " MUSIC_ALIAS, NULL, 0, NULL) == 0)
        music_paused = 0;
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
        DeleteFileA(song->path);
        song->used = 0;
        song->path[0] = '\0';
    }
}

int I_RegisterSong(void *data)
{
    unsigned char *midi_data;
    size_t midi_length;
    char temp_path[MAX_PATH];
    char temp_file[MAX_PATH];
    int i;

    midi_data = NULL;
    midi_length = 0;

    if (!ConvertMusToMidi(data, &midi_data, &midi_length))
        return 0;

    if (!GetTempPathA(MAX_PATH, temp_path))
    {
        free(midi_data);
        return 0;
    }

    if (!GetTempFileNameA(temp_path, "dmu", 0, temp_file))
    {
        free(midi_data);
        return 0;
    }

    if (!WriteMidiFile(temp_file, midi_data, midi_length))
    {
        free(midi_data);
        return 0;
    }

    free(midi_data);

    for (i = 0; i < MUSIC_SONG_LIMIT; ++i)
    {
        if (!music_songs[i].used)
        {
            music_songs[i].used = 1;
            strcpy(music_songs[i].path, temp_file);
            return i + 1;
        }
    }

    DeleteFileA(temp_file);
    return 0;
}

int I_QrySongPlaying(int handle)
{
    char mode[64];

    if (handle != music_current_handle)
        return 0;

    if (mciSendStringA("status " MUSIC_ALIAS " mode", mode, sizeof(mode), NULL) != 0)
        return 0;

    return strcmp(mode, "stopped") != 0;
}
