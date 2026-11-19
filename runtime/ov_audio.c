// nn::audio on the Switch 2 SDK: the audio renderer runs in the game's own process. The SDK links the renderer server
// (nn::audio::server) and the CPU command processor into the application; with AudioRendererParameter's rendering
// device set to CPU (+0x2c = 1) and execution mode to manual (+0x30 = 1), OpenAudioRenderer goes through the SDK's
// in-process manager instead of the aud:u service this host does not run, and ExecuteAudioRendererRendering renders a
// frame on the calling thread. In that mode the game's device sink reaches no device, so its mix is read through a
// circular-buffer sink added beside it and goes to the output stage of audio_mixer.c (the end of this file).
//
// The SDK's own functions are called at their guest definitions
// (ldr_symbol), which the overrides below do not change.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audren.h"
#include "host.h"
#include "platform/platform.h"

enum { P_SINKS = 0x14, P_DEVICE = 0x2c, P_MODE = 0x30, P_SIZE = 0x40 };
#define SYM_WORK_SIZE "_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE"
#define SYM_OPEN_EV "_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm"
#define SYM_CONFIG_SIZE "_ZN2nn5audio36GetAudioRendererConfigWorkBufferSizeERKNS0_22AudioRendererParameterE"
#define SYM_CONFIG_INIT "_ZN2nn5audio29InitializeAudioRendererConfigEPNS0_19AudioRendererConfigERKNS0_22AudioRendererParameterEPvm"

static uint64_t sdk(const char *name) {
    uint64_t a = ldr_symbol(name);
    if (!a) rt_fatal("audio: the SDK does not define %s", name);
    return a;
}

// The game's parameter with CPU rendering, manual execution and room for one more sink (the capture below), in
// guest-visible kernel memory for the SDK call. The renderer and its config must be given the same values.
static uint64_t cpu_manual_copy(const uint8_t *param) {
    static __thread uint8_t *copy;
    if (!copy) copy = mem_kernel_alloc(P_SIZE, 16);
    memcpy(copy, param, P_SIZE);
    int32_t one = 1, sinks;
    memcpy(copy + P_DEVICE, &one, 4);
    memcpy(copy + P_MODE, &one, 4);
    memcpy(&sinks, copy + P_SINKS, 4);
    sinks++;
    memcpy(copy + P_SINKS, &sinks, 4);
    return (uint64_t)(uintptr_t)copy;
}

static void log_param(const char *what, const uint8_t *p) {
    int32_t v[13];
    memcpy(v, p, sizeof v);
    rt_log("audio: %s: rate %d, %d samples/frame, %d mix buffers, %d submixes, %d voices, %d sinks, %d effects, "
           "%d perf frames, %d splitters; the game asks for device %d (%s), mode %d (%s)", what, v[0], v[1], v[2], v[3],
           v[4], v[5], v[6], v[7], v[9], v[11], v[11] ? "CPU" : "DSP", v[12], v[12] ? "manual" : "auto");
}

// size_t GetAudioRendererWorkBufferSize(const AudioRendererParameter &): the in-process renderer's requirement
static void ov_work_size(Cpu *c) {
    const uint8_t *param = arg_p(c, 0);
    static int logged;
    if (!logged++) log_param("GetAudioRendererWorkBufferSize", param);
    ret_x(c, rt_call(c, sdk(SYM_WORK_SIZE), 1, cpu_manual_copy(param)));
}

// size_t GetAudioRendererConfigWorkBufferSize(const AudioRendererParameter &)
static void ov_config_size(Cpu *c) { ret_x(c, rt_call(c, sdk(SYM_CONFIG_SIZE), 1, cpu_manual_copy(arg_p(c, 0)))); }

// The state of the capture: one renderer's mix on its way to the output
enum { CHANNELS_MAX = 6, READ_SAMPLES = 1920 };
static struct {
    struct Mixer *out;      // the output stage, opened with the renderer
    uint8_t *param;         // guest copy of the parameter the config was made with
    uint64_t config;        // the game's AudioRendererConfig
    uint64_t device_sink;   // the game's DeviceSinkType
    uint64_t sink;          // our CircularBufferSinkType
    int16_t *scratch;       // guest memory the samples are read into
    uint64_t scratch_bytes;
    int channels;           // of the game's final mix
    float downmix[4];       // front, centre, LFE, rear
} capture = {.downmix = {1.0f, 0.7071f, 0.0f, 0.7071f}};

// Result InitializeAudioRendererConfig(AudioRendererConfig *, const AudioRendererParameter &, void *, size_t)
static void ov_config_init(Cpu *c) {
    uint64_t config = arg_x(c, 0), work = arg_x(c, 2), size = arg_x(c, 3), param = cpu_manual_copy(arg_p(c, 1));
    if (!capture.param) capture.param = mem_kernel_alloc(P_SIZE, 16);
    memcpy(capture.param, (const void *)(uintptr_t)param, P_SIZE);
    ret_x(c, rt_call(c, sdk(SYM_CONFIG_INIT), 4, config, param, work, size));
}

// ---- the render thread
// The game asked for automatic execution, where the audio system renders a frame every 5 ms and signals the
// renderer's event; with manual execution that is the host's job. The thread is a guest thread made through the SDK's
// own nn::os::CreateThread, so it holds a virtual core and a priority like any other; its entry is a host function.
// The output device's clock sets the pace once it is playing, as the audio hardware does on the console, so its queue
// stays at its target and the two clocks cannot drift apart; a frame waits for the one before it to reach the queue.
#define SYM_EXECUTE "_ZN2nn5audio29ExecuteAudioRendererRenderingENS0_19AudioRendererHandleE"
#define SYM_CREATE_THREAD "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi"
#define SYM_START_THREAD "_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE"
#define SYM_SET_THREAD_NAME "_ZN2nn2os13SetThreadNameEPNS0_10ThreadTypeEPKc"
#define FRAME_NS 5000000ull            // 240 samples at 48 kHz
#define FRAME_GAP_NS 4000000ull        // device-paced frames are at least this far apart: the game's audio thread runs between them
#define RENDER_PRIORITY 9              // nn::os priority (0 highest): the audio system's own threads run high
#define RENDER_STACK 0x10000ull
#define THREAD_TYPE_BYTES 0x400

typedef struct { uint64_t handle[2]; volatile int open; } Renderer;
static Renderer renderers[4];

static void render_main(Cpu *c) {
    Renderer *r = &renderers[arg_x(c, 0)];
    uint64_t exec = sdk(SYM_EXECUTE), next = kern_now_ns(), last = 0;
    rt_log("audio: render thread started (a frame every 5 ms)");
    while (r->open) {
        uint64_t now = kern_now_ns(), wait = 0;
        int64_t pace = capture.out ? audio_output_pace_ns(capture.out, now) : -1;
        if (pace > 0) wait = (uint64_t)pace;
        else if (pace == 0) wait = last && now - last < FRAME_GAP_NS ? FRAME_GAP_NS - (now - last) : 0;
        else if (next > now) wait = next - now;
        if (wait) {
            int away = kern_core_away();
            plat_wait_ns(wait);
            kern_core_back(&away);
            if (pace >= 0) continue;   // look at the queue again
        }
        now = kern_now_ns();
        if (pace == 0) next = now + FRAME_NS;
        else if (now - next > 20 * FRAME_NS) next = now;   // fell far behind (a debugger, a stall): no burst to catch up
        else next += FRAME_NS;
        last = now;
        rt_call(c, exec, 2, r->handle[0], r->handle[1]);   // generate commands, mix on the CPU, signal the event
    }
}

static void start_render_thread(Cpu *c, uint64_t handle) {
    int slot = -1;
    for (int i = 0; i < 4 && slot < 0; i++)
        if (!renderers[i].open) slot = i;
    if (slot < 0) rt_fatal("audio: more than 4 audio renderers");
    Renderer *r = &renderers[slot];
    memcpy(r->handle, (const void *)(uintptr_t)handle, sizeof r->handle);
    r->open = 1;
    // From the game's own allocator: nn::os maps a thread's stack into the stack region (svcMapMemory), which takes
    // heap or alias memory only, not the host's kernel region.
    uint64_t alloc = sdk("aligned_alloc");
    uint64_t thread = rt_call(c, alloc, 2, (uint64_t)64, (uint64_t)THREAD_TYPE_BYTES);
    uint64_t stack = rt_call(c, alloc, 2, (uint64_t)0x1000, RENDER_STACK);
    if (!thread || !stack) rt_fatal("audio: the game's allocator gave no memory for the render thread");
    memset((void *)(uintptr_t)thread, 0, THREAD_TYPE_BYTES);
    uint64_t entry = hostfn_register("<audio render thread>", render_main);
    uint64_t res = rt_call(c, sdk(SYM_CREATE_THREAD), 6, thread, entry, (uint64_t)slot, stack, RENDER_STACK,
                           (uint64_t)RENDER_PRIORITY);
    if ((uint32_t)res) rt_fatal("audio: nn::os::CreateThread for the render thread failed (%#x)", (uint32_t)res);
    static const char name[] = "MtfAudioRender";
    char *gname = mem_kernel_alloc(sizeof name, 16);
    memcpy(gname, name, sizeof name);
    rt_call(c, sdk(SYM_SET_THREAD_NAME), 2, thread, (uint64_t)(uintptr_t)gname);
    rt_call(c, sdk(SYM_START_THREAD), 1, thread);
}

// Result OpenAudioRenderer(AudioRendererHandle *, os::SystemEvent *, const AudioRendererParameter &, void *, size_t)
static void ov_open(Cpu *c) {
    uint64_t handle = arg_x(c, 0), event = arg_x(c, 1), work = arg_x(c, 3), size = arg_x(c, 4);
    const uint8_t *param = arg_p(c, 2);
    log_param("OpenAudioRenderer", param);
    int32_t mode, rate, samples;
    memcpy(&mode, param + P_MODE, 4);
    memcpy(&rate, param, 4);
    memcpy(&samples, param + 4, 4);
    uint64_t r = rt_call(c, sdk(SYM_OPEN_EV), 5, handle, event, cpu_manual_copy(param), work, size);
    rt_log("audio: OpenAudioRenderer (in-process CPU renderer) -> %#llx", (unsigned long long)(uint32_t)r);
    if (!(uint32_t)r && !capture.out) capture.out = audio_output_open(rate, samples);
    if (!(uint32_t)r && mode == 0) start_render_thread(c, handle);   // the game asked for automatic execution
    ret_x(c, r);
}

// ---- the capture: what the game's device sink would have played
// Every device sink of the game gets a twin, a circular-buffer sink on the same final-mix channels, in a memory pool of
// its own. The SDK stores each rendered frame in that ring (per channel, 240 samples each); RequestUpdate brings back how
// far the renderer has written, and right after it, on the game's own audio thread, the new samples are read interleaved
// and sent to the output. The game's 5.1 mix follows the output's layout: the same six channels, or for two speakers
// the coefficients the game gave its device sink, as the console's downmix uses them.
#define SYM_ADD_DEVICE_SINK "_ZN2nn5audio13AddDeviceSinkEPNS0_19AudioRendererConfigEPNS0_14DeviceSinkTypeEPNS0_12FinalMixTypeEPKaiPKc"
#define SYM_ADD_CIRCULAR "_ZN2nn5audio21AddCircularBufferSinkEPNS0_19AudioRendererConfigEPNS0_22CircularBufferSinkTypeEPNS0_12FinalMixTypeEPKaiPvmNS0_12SampleFormatE"
#define SYM_CIRCULAR_SIZE "_ZN2nn5audio42GetRequiredBufferSizeForCircularBufferSinkEPKNS0_22AudioRendererParameterEiiNS0_12SampleFormatE"
#define SYM_READ_CIRCULAR "_ZN2nn5audio22ReadCircularBufferSinkEPNS0_22CircularBufferSinkTypeEPvmNS1_18InterleavingFormatE"
#define SYM_ACQUIRE_POOL "_ZN2nn5audio17AcquireMemoryPoolEPNS0_19AudioRendererConfigEPNS0_14MemoryPoolTypeEPvm"
#define SYM_ATTACH_POOL "_ZN2nn5audio23RequestAttachMemoryPoolEPNS0_14MemoryPoolTypeE"
#define SYM_UPDATE "_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE"
#define SYM_SET_DOWNMIX "_ZN2nn5audio19SetDownMixParameterEPNS0_14DeviceSinkTypeEPKNS1_16DownMixParameterE"
enum { INTERLEAVED = 1, RING_FRAMES = 32, POOL_ALIGN = 0x1000, SDK_OBJECT_BYTES = 0x100 };

static void capture_start(Cpu *c, uint64_t config, uint64_t final_mix, const int8_t *inputs, int channels) {
    if (channels < 1 || channels > CHANNELS_MAX || !capture.param) {
        rt_log("audio: a device sink of %d channels cannot be captured", channels);
        return;
    }
    int32_t samples;
    memcpy(&samples, capture.param + 4, 4);
    if (samples <= 0 || samples > READ_SAMPLES) {
        rt_log("audio: %d samples per frame cannot be captured", samples);
        return;
    }
    uint64_t input_copy = (uint64_t)(uintptr_t)mem_kernel_alloc(CHANNELS_MAX, 16);
    memcpy((void *)(uintptr_t)input_copy, inputs, (size_t)channels);
    // The ring must hold whole frames, or reads would start inside one; the pool around it takes whole pages.
    uint64_t ring_bytes = rt_call(c, sdk(SYM_CIRCULAR_SIZE), 4, (uint64_t)(uintptr_t)capture.param, (uint64_t)channels,
                                  (uint64_t)RING_FRAMES, (uint64_t)FMT_PCM16);
    uint64_t pool_bytes = (ring_bytes + POOL_ALIGN - 1) & ~(uint64_t)(POOL_ALIGN - 1);
    uint64_t ring = rt_call(c, sdk("aligned_alloc"), 2, (uint64_t)POOL_ALIGN, pool_bytes);
    uint64_t pool = (uint64_t)(uintptr_t)mem_kernel_alloc(SDK_OBJECT_BYTES, 16);
    uint64_t sink = (uint64_t)(uintptr_t)mem_kernel_alloc(SDK_OBJECT_BYTES, 16);
    if (!ring_bytes || !ring) {
        rt_log("audio: no memory for the capture ring (%llu bytes)", (unsigned long long)ring_bytes);
        return;
    }
    memset((void *)(uintptr_t)ring, 0, pool_bytes);
    if (!(rt_call(c, sdk(SYM_ACQUIRE_POOL), 4, config, pool, ring, pool_bytes) & 1)) {
        rt_log("audio: the capture's memory pool was refused");
        return;
    }
    rt_call(c, sdk(SYM_ATTACH_POOL), 1, pool);
    uint64_t r = rt_call(c, sdk(SYM_ADD_CIRCULAR), 8, config, sink, final_mix, input_copy, (uint64_t)channels, ring,
                         ring_bytes, (uint64_t)FMT_PCM16);
    if ((uint32_t)r) {
        rt_log("audio: AddCircularBufferSink failed (%#x): the game will be silent", (uint32_t)r);
        return;
    }
    if (!capture.scratch) capture.scratch = mem_kernel_alloc(READ_SAMPLES * CHANNELS_MAX * sizeof(int16_t), 16);
    capture.scratch_bytes = (uint64_t)(READ_SAMPLES / samples * samples) * (uint64_t)channels * sizeof(int16_t);
    capture.channels = channels;
    capture.sink = sink;
    capture.config = config;
    rt_log("audio: the device sink's %d channels are captured through a ring of %llu bytes (%d frames)", channels,
           (unsigned long long)ring_bytes, RING_FRAMES);
}

// Result AddDeviceSink(AudioRendererConfig *, DeviceSinkType *, FinalMixType *, const int8_t *input, int count, const char *name)
static void ov_add_device_sink(Cpu *c) {
    uint64_t config = arg_x(c, 0), sink = arg_x(c, 1), final_mix = arg_x(c, 2), input = arg_x(c, 3), name = arg_x(c, 5);
    int channels = (int)arg_x(c, 4);
    uint64_t r = rt_call(c, sdk(SYM_ADD_DEVICE_SINK), 6, config, sink, final_mix, input, (uint64_t)channels, name);
    const int8_t *in = (const int8_t *)(uintptr_t)input;
    rt_log("audio: AddDeviceSink '%s', %d channels (mix buffers %d %d %d %d %d %d) -> %#x",
           name ? (const char *)(uintptr_t)name : "", channels, in[0], channels > 1 ? in[1] : -1, channels > 2 ? in[2] : -1,
           channels > 3 ? in[3] : -1, channels > 4 ? in[4] : -1, channels > 5 ? in[5] : -1, (uint32_t)r);
    if (!(uint32_t)r && capture.config != config) {
        capture.device_sink = sink;
        capture_start(c, config, final_mix, in, channels);
    }
    ret_x(c, r);
}

// void SetDownMixParameter(DeviceSinkType *, const DeviceSinkType::DownMixParameter *): four floats
static void ov_set_downmix(Cpu *c) {
    uint64_t sink = arg_x(c, 0), param = arg_x(c, 1);
    if (sink == capture.device_sink && param) {
        memcpy(capture.downmix, (const void *)(uintptr_t)param, sizeof capture.downmix);
        rt_log("audio: the game's downmix: %.4f %.4f %.4f %.4f", capture.downmix[0], capture.downmix[1], capture.downmix[2],
               capture.downmix[3]);
    }
    rt_call(c, sdk(SYM_SET_DOWNMIX), 2, sink, param);
}

// MTF_TRACE=audio: how loud each channel of the game's mix is, one line per 5 s, before any downmix or volume
static void capture_channel_levels(const int16_t *in, int frames) {
    static const char *const names[CHANNELS_MAX] = {"FL", "FR", "C", "LFE", "RL", "RR"};
    static double sum[CHANNELS_MAX];
    static int peak[CHANNELS_MAX], counted;
    const int n = capture.channels;
    for (int k = 0; k < frames; k++, in += n)
        for (int c = 0; c < n; c++) {
            sum[c] += (double)in[c] * in[c];
            if (abs(in[c]) > peak[c]) peak[c] = abs(in[c]);
        }
    counted += frames;
    if (counted < 5 * 48000) return;
    char line[256];
    int at = 0;
    for (int c = 0; c < n; c++) {
        at += snprintf(line + at, sizeof line - (size_t)at, " %s rms %.4f peak %.4f;", n == 6 ? names[c] : "ch",
                       sqrt(sum[c] / counted) / 32768.0, peak[c] / 32768.0);
        sum[c] = 0, peak[c] = 0;
    }
    counted = 0;
    rt_log("audio: game mix per channel:%s", line);
}

// `frames` interleaved frames of the game's layout, to the output's layout and volume
static void capture_output(const int16_t *in, int frames) {
    static float block[READ_SAMPLES * CHANNELS_MAX];
    if (rt_trace & RT_TRACE_AUDIO) capture_channel_levels(in, frames);
    const int n = capture.channels, oc = audio_output_channels(capture.out);
    const float scale = audio_output_volume() / 32768.0f, *d = capture.downmix;
    float *o = block;
    for (int k = 0; k < frames; k++, in += n, o += oc) {
        float s[CHANNELS_MAX] = {0};
        if (n >= 6 && oc == 2) {
            s[0] = d[0] * in[0] + d[1] * in[2] + d[2] * in[3] + d[3] * in[4];
            s[1] = d[0] * in[1] + d[1] * in[2] + d[2] * in[3] + d[3] * in[5];
        } else if (n == 1) {
            s[0] = s[1] = in[0];
        } else {
            for (int c = 0; c < n && c < oc; c++) s[c] = in[c];
        }
        for (int c = 0; c < oc; c++) {
            float x = s[c] * scale;
            o[c] = x > 1 ? 1 : x < -1 ? -1 : x;
        }
    }
    audio_output_write(capture.out, block, frames);
}

// Result RequestUpdateAudioRenderer(AudioRendererHandle, const AudioRendererConfig *): the handle takes two registers
static void ov_update(Cpu *c) {
    static uint64_t update, read;
    if (!update) update = sdk(SYM_UPDATE), read = sdk(SYM_READ_CIRCULAR);
    uint64_t config = arg_x(c, 2);
    uint64_t r = rt_call(c, update, 3, arg_x(c, 0), arg_x(c, 1), config);
    if (!(uint32_t)r && capture.sink && config == capture.config) {
        const uint64_t frame_bytes = (uint64_t)capture.channels * sizeof(int16_t);
        for (;;) {
            uint64_t got = rt_call(c, read, 4, capture.sink, (uint64_t)(uintptr_t)capture.scratch, capture.scratch_bytes,
                                   (uint64_t)INTERLEAVED);
            if (got >= frame_bytes) capture_output(capture.scratch, (int)(got / frame_bytes));
            if (got < capture.scratch_bytes) break;
        }
    }
    ret_x(c, r);
}

HOST_OVERRIDE(SYM_WORK_SIZE, ov_work_size);
HOST_OVERRIDE(SYM_OPEN_EV, ov_open);
HOST_OVERRIDE(SYM_CONFIG_SIZE, ov_config_size);
HOST_OVERRIDE(SYM_CONFIG_INIT, ov_config_init);
HOST_OVERRIDE(SYM_ADD_DEVICE_SINK, ov_add_device_sink);
HOST_OVERRIDE(SYM_SET_DOWNMIX, ov_set_downmix);
HOST_OVERRIDE(SYM_UPDATE, ov_update);
