// nn::audio::AudioOut and the audio device queries, on SDL3. The game renders its mix on the CPU (ov_audio.c) and
// appends PCM blocks to an AudioOut; on the console the audout service plays them and hands them back through a
// release event. Here each AudioOut is an SDL audio stream in the game's own format (PCM16, 48 kHz, the channel count
// it asked for: MTF's renderer mixes 5.1), and SDL converts to the output the player chose:
//   MTF_AUDIO_CHANNELS=auto    the Windows default device's own layout (a 5.1 receiver gets 5.1, headphones stereo)
//   MTF_AUDIO_CHANNELS=stereo  downmixed to two channels, as the Switch 2's stereo setting does
//   MTF_AUDIO_CHANNELS=5.1     six channels, as its surround setting does
#include <SDL3/SDL.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "ov.h"
#include "platform/platform.h"

enum { FMT_PCM16 = 2, STATE_STARTED = 0, STATE_STOPPED = 1, RATE = 48000, QMAX = 64, MAX_OUTS = 8, DEVNAME = 256 };

// The first 24 bytes of a guest AudioOutBuffer, which SetAudioOutBufferInfo fills and only the host reads.
typedef struct { uint64_t ptr, cap, size; } GuestBuffer;

typedef struct {
    uint64_t guest;               // the game's AudioOut object
    uint64_t event;               // its release event (0: the game polls)
    int channels;
    SDL_AudioStream *stream;
    volatile int state;
    float volume;
    PlatMutex lock;
    uint64_t queue[QMAX];         // appended, not yet played (guest AudioOutBuffer addresses)
    unsigned q_head, q_tail;
    uint64_t played_bytes;        // of queue[q_head]
    uint64_t released[QMAX];
    unsigned r_head, r_tail;
} Out;

static Out outs[MAX_OUTS];
static PlatMutex table_lock = PLAT_MUTEX_INIT;
static int sdl_audio_ok = -1;

static int want_channels(void) {
    const char *e = cfg_value("MTF_AUDIO_CHANNELS");
    if (!strcmp(e, "stereo")) return 2;
    if (!strcmp(e, "5.1")) return 6;
    return 0;   // auto: the device's
}

static Out *out_of(uint64_t guest) {
    plat_mutex_lock(&table_lock);
    Out *found = NULL;
    for (int i = 0; i < MAX_OUTS && !found; i++)
        if (outs[i].guest == guest) found = &outs[i];
    plat_mutex_unlock(&table_lock);
    if (!found) rt_fatal("audio: AudioOut %#llx was never opened", (unsigned long long)guest);
    return found;
}

// SDL's audio thread asks for more: copy from the queued guest buffers, apply the volume, release finished buffers.
static void SDLCALL pull(void *user, SDL_AudioStream *stream, int additional, int total) {
    (void)total;
    Out *o = user;
    int16_t chunk[2048];
    while (additional > 0) {
        int want = additional < (int)sizeof chunk ? additional : (int)sizeof chunk, got = 0, released = 0;
        plat_mutex_lock(&o->lock);
        while (got < want && o->state == STATE_STARTED && o->q_head != o->q_tail) {
            uint64_t gb = o->queue[o->q_head % QMAX];
            const GuestBuffer *b = (const GuestBuffer *)(uintptr_t)gb;
            uint64_t left = b->size - o->played_bytes;
            int n = left < (uint64_t)(want - got) ? (int)left : want - got;
            memcpy((uint8_t *)chunk + got, (const uint8_t *)(uintptr_t)b->ptr + o->played_bytes, (size_t)n);
            got += n;
            o->played_bytes += (uint64_t)n;
            if (o->played_bytes >= b->size) {
                o->q_head++;
                o->played_bytes = 0;
                o->released[o->r_tail++ % QMAX] = gb;
                released = 1;
            }
        }
        float vol = o->volume;
        plat_mutex_unlock(&o->lock);
        if (released && o->event) kern_signal_system_event(o->event);
        if (!got) {   // starved or stopped: silence, so the device keeps its clock
            memset(chunk, 0, (size_t)want);
            got = want;
        } else if (vol != 1.0f) {
            for (int i = 0; i < got / 2; i++) {
                float s = chunk[i] * vol;
                chunk[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
            }
        }
        SDL_PutAudioStreamData(stream, chunk, got);
        additional -= got;
    }
}

static int open_stream(Out *o) {
    if (sdl_audio_ok < 0) {
        sdl_audio_ok = SDL_InitSubSystem(SDL_INIT_AUDIO);
        if (!sdl_audio_ok) rt_log("audio: SDL audio init failed (%s): playing into a silent sink", SDL_GetError());
    }
    if (!sdl_audio_ok) return 0;
    SDL_AudioSpec src = {SDL_AUDIO_S16, o->channels, RATE};
    SDL_AudioSpec dev = {SDL_AUDIO_F32, want_channels(), RATE};
    SDL_AudioDeviceID id = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, dev.channels ? &dev : NULL);
    if (!id) {
        rt_log("audio: no output device (%s)", SDL_GetError());
        return 0;
    }
    o->stream = SDL_CreateAudioStream(&src, NULL);
    if (!o->stream || !SDL_SetAudioStreamGetCallback(o->stream, pull, o) || !SDL_BindAudioStream(id, o->stream)) {
        rt_log("audio: cannot start the output stream (%s)", SDL_GetError());
        return 0;
    }
    SDL_AudioSpec out;
    SDL_GetAudioDeviceFormat(id, &out, NULL);
    rt_log("audio: AudioOut %d channels -> output device %d channels at %d Hz (MTF_AUDIO_CHANNELS=%s)", o->channels,
           out.channels, out.freq, cfg_value("MTF_AUDIO_CHANNELS"));
    return 1;
}

// The front of the guest AudioOut, as the SDK fills it from the service: rate, channels, format, state
static void write_info(Out *o) {
    uint32_t *f = (uint32_t *)(uintptr_t)o->guest;
    f[0] = RATE, f[1] = (uint32_t)o->channels, f[2] = FMT_PCM16, f[3] = (uint32_t)o->state;
}

// AudioOutParameter = { int32 sample rate; uint16 channel count; ... }, 0 = the default (48 kHz, 2 channels)
static void init_param(Cpu *c) { memset(arg_p(c, 0), 0, 8); }

static void list_outs(Cpu *c) {
    char *info = arg_p(c, 0);
    if ((int)arg_x(c, 1) < 1 || !info) {
        ret_x(c, 0);
        return;
    }
    memset(info, 0, 256);
    strcpy(info, "DeviceOut");
    ret_x(c, 1);
}

static void open_out(Cpu *c, uint64_t guest, uint64_t event, const uint8_t *param) {
    int32_t rate;
    uint16_t channels;
    memcpy(&rate, param, 4);
    memcpy(&channels, param + 4, 2);
    if (!channels) channels = 2;
    if (rate && rate != RATE) rt_fatal("audio: AudioOut at %d Hz (48 kHz only)", rate);
    plat_mutex_lock(&table_lock);
    Out *o = NULL;
    for (int i = 0; i < MAX_OUTS && !o; i++)
        if (!outs[i].guest) o = &outs[i];
    if (!o) rt_fatal("audio: more than %d AudioOuts open", MAX_OUTS);
    memset(o, 0, sizeof *o);
    o->guest = guest;
    plat_mutex_unlock(&table_lock);
    o->lock = (PlatMutex)PLAT_MUTEX_INIT;
    o->event = event;
    o->channels = channels;
    o->volume = 1.0f;
    o->state = STATE_STOPPED;
    if (event) kern_bind_system_event(c, event, 1);
    open_stream(o);
    write_info(o);
    rt_log("audio: OpenAudioOut %d channels%s", channels, event ? "" : " (no release event)");
}

// Result OpenAudioOut(AudioOut *, os::SystemEvent *, const char *name, const AudioOutParameter &)
static void ov_open(Cpu *c) {
    open_out(c, arg_x(c, 0), arg_x(c, 1), arg_p(c, 3));
    ret_x(c, 0);
}

static void ov_start(Cpu *c) {
    Out *o = out_of(arg_x(c, 0));
    o->state = STATE_STARTED;
    write_info(o);
    if (o->stream) SDL_ResumeAudioStreamDevice(o->stream);
    ret_x(c, 0);
}

static void ov_stop(Cpu *c) {
    Out *o = out_of(arg_x(c, 0));
    plat_mutex_lock(&o->lock);
    o->state = STATE_STOPPED;
    while (o->q_head != o->q_tail) o->released[o->r_tail++ % QMAX] = o->queue[o->q_head++ % QMAX];   // all back
    o->played_bytes = 0;
    plat_mutex_unlock(&o->lock);
    write_info(o);
    if (o->event) kern_signal_system_event(o->event);
}

static void ov_close(Cpu *c) {
    Out *o = out_of(arg_x(c, 0));
    if (o->stream) SDL_DestroyAudioStream(o->stream);
    o->stream = NULL;
    o->state = STATE_STOPPED;
    write_info(o);
    plat_mutex_lock(&table_lock);
    o->guest = 0;
    plat_mutex_unlock(&table_lock);
}

static void ov_rate(Cpu *c) { (void)out_of(arg_x(c, 0)); ret_x(c, RATE); }
static void ov_channels(Cpu *c) { ret_x(c, (uint64_t)out_of(arg_x(c, 0))->channels); }
static void ov_format(Cpu *c) { (void)out_of(arg_x(c, 0)); ret_x(c, FMT_PCM16); }
static void ov_set_volume(Cpu *c) {
    Out *o = out_of(arg_x(c, 0));
    float v = arg_s(c, 0);
    o->volume = v >= 0 ? v : 0;
}

static void ov_set_buffer(Cpu *c) {
    GuestBuffer *b = arg_p(c, 0);
    b->ptr = arg_x(c, 1), b->cap = arg_x(c, 2), b->size = arg_x(c, 3);
}
static void ov_buffer_ptr(Cpu *c) { ret_x(c, ((const GuestBuffer *)arg_p(c, 0))->ptr); }
static void ov_buffer_size(Cpu *c) { ret_x(c, ((const GuestBuffer *)arg_p(c, 0))->size); }

// bool AppendAudioOutBuffer(AudioOut *, AudioOutBuffer *): false when the queue is full
static void ov_append(Cpu *c) {
    Out *o = out_of(arg_x(c, 0));
    plat_mutex_lock(&o->lock);
    int ok = o->q_tail - o->q_head < QMAX;
    if (ok) o->queue[o->q_tail++ % QMAX] = arg_x(c, 1);
    plat_mutex_unlock(&o->lock);
    ret_x(c, (uint64_t)ok);
}

// AudioOutBuffer *GetReleasedAudioOutBuffer(AudioOut *): a played buffer, NULL when none
static void ov_released(Cpu *c) {
    Out *o = out_of(arg_x(c, 0));
    plat_mutex_lock(&o->lock);
    uint64_t b = o->r_head != o->r_tail ? o->released[o->r_head++ % QMAX] : 0;
    plat_mutex_unlock(&o->lock);
    ret_x(c, b);
}

// ---- audio devices
// int ListAudioOutputDeviceName(AudioDeviceName *, int): the console's output in use, by operation mode
static void ov_list_device_names(Cpu *c) {
    char *names = arg_p(c, 0);
    if ((int)arg_x(c, 1) <= 0 || !names) {
        ret_x(c, 0);
        return;
    }
    memset(names, 0, DEVNAME);
    strcpy(names, ov_handheld() ? "AudioBuiltInSpeakerOutput" : "AudioTvOutput");
    ret_x(c, 1);
}
static void ov_auto_tune(Cpu *c) { (void)c; }   // device volume auto-tune: the Windows mixer sets the level

HOST_OVERRIDE("_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", init_param);
HOST_OVERRIDE("_ZN2nn5audio13ListAudioOutsEPNS0_12AudioOutInfoEi", list_outs);
HOST_OVERRIDE("_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventEPKcRKNS0_17AudioOutParameterE", ov_open);
HOST_OVERRIDE("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", ov_start);
HOST_OVERRIDE("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", ov_stop);
HOST_OVERRIDE("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", ov_close);
HOST_OVERRIDE("_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", ov_rate);
HOST_OVERRIDE("_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", ov_channels);
HOST_OVERRIDE("_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", ov_format);
HOST_OVERRIDE("_ZN2nn5audio17SetAudioOutVolumeEPNS0_8AudioOutEf", ov_set_volume);
HOST_OVERRIDE("_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", ov_set_buffer);
HOST_OVERRIDE("_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", ov_buffer_ptr);
HOST_OVERRIDE("_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", ov_buffer_size);
HOST_OVERRIDE("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", ov_append);
HOST_OVERRIDE("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", ov_released);
HOST_OVERRIDE("_ZN2nn5audio25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEi", ov_list_device_names);
HOST_OVERRIDE("_ZN2nn5audio40SetDeviceOutputVolumeAutoTuneModeEnabledEb", ov_auto_tune);
