// nn::hid for MTF: controller state, six-axis sensor, vibration and the controller applets, answered from
// the host input (input.c) as one player's controller. On the console these read hid shared memory or call the hid
// service, neither of which the host has.
//
// One controller is connected: player 1, as a Pro Controller (full key, on npad No1) when docked, or the console's own
// Joy-Cons (handheld, npad 0x20) with MTF_OPERATION_MODE=handheld.
#include <math.h>
#include <string.h>

#include "host.h"
#include "input.h"
#include "ov.h"
#include "platform/platform.h"

enum { NPAD_NO1 = 0, NPAD_HANDHELD = 0x20 };
enum { STYLE_FULLKEY = 1u << 0, STYLE_HANDHELD = 1u << 1, STYLE_JOYDUAL = 1u << 2, STYLE_JOYLEFT = 1u << 3,
       STYLE_JOYRIGHT = 1u << 4 };
enum { ATTR_CONNECTED = 1u << 0, ATTR_WIRED = 1u << 1, ATTR_LEFT_CONN = 1u << 2, ATTR_LEFT_WIRED = 1u << 3,
       ATTR_RIGHT_CONN = 1u << 4, ATTR_RIGHT_WIRED = 1u << 5 };
#define STICK_THRESH 0x4000            // stick direction pseudo buttons (16..23) past half travel
#define SIX_PERIOD_NS 5000000ull       // six-axis samples 5 ms apart
#define SHAKE_G 3.0                    // the shake: side-to-side acceleration, G
#define SHAKE_HZ 6.0
#define SHAKE_REV 1.5                  // its yaw rate, revolutions/s
#define RES_OK 0u
#define RES_CTRL_CANCELED (202u | 3101u << 9)

// nn::hid::NpadFullKeyState and the other styles' states share one layout.
typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, pad; } NpadState;
_Static_assert(sizeof(NpadState) == 0x28, "NpadState");

static volatile int64_t sampling;
static volatile uint32_t styles_supported = 0xffffffffu;
static volatile uint64_t shake_t0;

static uint32_t connected_id(void) { return ov_handheld() ? NPAD_HANDHELD : NPAD_NO1; }
static uint32_t style_of(uint32_t id) {
    if (id != connected_id()) return 0;
    return id == NPAD_HANDHELD ? STYLE_HANDHELD : STYLE_FULLKEY;
}

static void fill(NpadState *s, uint32_t id, uint32_t style) {
    memset(s, 0, sizeof *s);
    s->sampling = __atomic_add_fetch(&sampling, 1, __ATOMIC_RELAXED);
    if (!(style_of(id) & style)) return;   // no controller of this id and style: attributes 0
    uint64_t b;
    input_poll(&b, &s->lx, &s->ly, &s->rx, &s->ry);
    b &= 0xffffull;
    if (s->lx < -STICK_THRESH) b |= 1u << 16;
    if (s->ly > STICK_THRESH) b |= 1u << 17;
    if (s->lx > STICK_THRESH) b |= 1u << 18;
    if (s->ly < -STICK_THRESH) b |= 1u << 19;
    if (s->rx < -STICK_THRESH) b |= 1u << 20;
    if (s->ry > STICK_THRESH) b |= 1u << 21;
    if (s->rx > STICK_THRESH) b |= 1u << 22;
    if (s->ry < -STICK_THRESH) b |= 1u << 23;
    s->buttons = b;
    s->attributes = ATTR_CONNECTED | (id == NPAD_HANDHELD ? ATTR_WIRED | ATTR_LEFT_CONN | ATTR_LEFT_WIRED |
                                                               ATTR_RIGHT_CONN | ATTR_RIGHT_WIRED
                                                         : ATTR_LEFT_CONN | ATTR_RIGHT_CONN);
}

static uint32_t id_arg(Cpu *c, int i) {
    const uint32_t *p = arg_p(c, i);
    return p ? *p : 0xffffffffu;
}

static void state_full_key(Cpu *c) { fill(arg_p(c, 0), id_arg(c, 1), STYLE_FULLKEY); }
static void state_handheld(Cpu *c) { fill(arg_p(c, 0), id_arg(c, 1), STYLE_HANDHELD); }
static void state_joy_dual(Cpu *c) { fill(arg_p(c, 0), id_arg(c, 1), STYLE_JOYDUAL); }
static void state_joy_left(Cpu *c) { fill(arg_p(c, 0), id_arg(c, 1), STYLE_JOYLEFT); }
static void state_joy_right(Cpu *c) { fill(arg_p(c, 0), id_arg(c, 1), STYLE_JOYRIGHT); }

static void initialize_npad(Cpu *c) {
    (void)c;
    rt_log("hid: player 1 on npad %#x (%s)", connected_id(), ov_handheld() ? "handheld" : "Pro Controller");
}
static void set_styles(Cpu *c) { __atomic_store_n(&styles_supported, (uint32_t)arg_x(c, 0), __ATOMIC_RELAXED); }
static void get_styles(Cpu *c) { ret_x(c, __atomic_load_n(&styles_supported, __ATOMIC_RELAXED)); }
static void get_style(Cpu *c) { ret_x(c, style_of(id_arg(c, 0))); }
static void nothing(Cpu *c) { (void)c; }
static void ok(Cpu *c) { ret_x(c, RES_OK); }

// Single Joy-Con assignment returning the other half's id: there is no other half (false)
static void joy_single_out(Cpu *c) { ret_x(c, 0); }

// ControllerSupportArg starts { int8 min, max }; ControllerSupportResultInfo is { int count; uint32 selected id }.
// With fewer players than the minimum the console's applet waits, and closing it reads as canceled.
static void controller_support(Cpu *c) {
    int32_t *info = arg_p(c, 0);
    const int8_t *arg = arg_p(c, 1);
    int count = 1, canceled = arg && arg[0] > count;
    if (arg) rt_log("hid: controller applet wants %d to %d players, 1 connected%s", arg[0], arg[1],
                    canceled ? ": too few, closed as canceled" : "");
    if (info) {
        info[0] = count;
        info[1] = (int32_t)connected_id();
    }
    ret_x(c, canceled ? RES_CTRL_CANCELED : RES_OK);
}

// Pro Controller grey body, black buttons
static void color(uint32_t *c) {
    if (c) c[0] = 0xff828282u, c[1] = 0xff0f0f0fu;
}
static void controller_color(Cpu *c) {
    color(arg_p(c, 0));
    ret_x(c, RES_OK);
}
static void controller_color2(Cpu *c) {
    color(arg_p(c, 0));
    color(arg_p(c, 1));
    ret_x(c, RES_OK);
}

// ---- six-axis sensor
// SixAxisSensorHandle: byte 0 the style's bit number, byte 1 the npad id, byte 2 the device (left or right Joy-Con of a
// pair). Two-Joy-Con styles (handheld, dual) have two sensors, the others one.
static void six_handles(Cpu *c) {
    uint32_t *h = arg_p(c, 0);
    int n = (int)arg_x(c, 1);
    uint32_t id = id_arg(c, 2), style = (uint32_t)arg_x(c, 3);
    int k = style & (STYLE_HANDHELD | STYLE_JOYDUAL) ? 2 : 1;
    if (k > n) k = n;
    for (int i = 0; i < k; i++) h[i] = (uint32_t)(style ? __builtin_ctz(style) : 0) | (id & 0xff) << 8 | (uint32_t)i << 16;
    ret_x(c, (uint64_t)(k > 0 ? k : 0));
}

static int six_connected(uint32_t h) { return (h & 0xff) < 32 && (style_of(h >> 8 & 0xff) >> (h & 0xff) & 1); }

static int shaking(void) {
    int held = input_shake_held();
    uint64_t t0 = __atomic_load_n(&shake_t0, __ATOMIC_RELAXED);
    if (held && !t0) __atomic_store_n(&shake_t0, kern_now_ns(), __ATOMIC_RELAXED);
    if (!held && t0) __atomic_store_n(&shake_t0, 0, __ATOMIC_RELAXED);
    return held;
}

// At rest the controller lies flat: gravity (0, 0, -1) G, no rotation, identity direction. The gamepad's sensors replace
// that when it has them (motion_fusion.c); the shake key adds a side-to-side swing on top.
static void six_sample(MotionState *s, int64_t samp, const MotionState *live, int shake) {
    if (live) *s = *live;
    else {
        memset(s, 0, sizeof *s);
        s->acc[2] = -1;
        s->dir[0] = s->dir[4] = s->dir[8] = 1;
    }
    s->delta_ns = (int64_t)SIX_PERIOD_NS;
    s->sampling = samp;
    s->attributes = 1;
    if (shake) {
        double ph = 2 * M_PI * SHAKE_HZ * (double)(kern_now_ns() - __atomic_load_n(&shake_t0, __ATOMIC_RELAXED)) / 1e9;
        s->acc[0] += (float)(SHAKE_G * sin(ph));
        s->gyro[2] += (float)(SHAKE_REV * sin(ph));
    }
}

static int six_states(MotionState *out, int n, uint32_t h) {
    if (n <= 0) return 0;
    if (!six_connected(h)) {
        memset(out, 0, sizeof *out * (size_t)n);
        return 0;
    }
    int64_t samp = (int64_t)(kern_now_ns() / SIX_PERIOD_NS);
    MotionState live;
    int have = motion_read(motion_global(), &live, 1, kern_now_ns()) > 0, shake = shaking();
    for (int i = 0; i < n; i++) six_sample(&out[i], samp - i, have ? &live : NULL, shake);
    return n;
}

static void six_state(Cpu *c) { six_states(arg_p(c, 0), 1, id_arg(c, 1)); }
static void six_states_n(Cpu *c) { ret_x(c, (uint64_t)six_states(arg_p(c, 0), (int)arg_x(c, 1), id_arg(c, 2))); }

// ---- vibration
// VibrationDeviceHandle: byte 0 the SDK style index (full key 3, handheld 4), byte 1 the npad id, byte 2 the motor
// (0 left, 1 right). VibrationValue { float amplitude_low, frequency_low, amplitude_high, frequency_high }.
typedef struct { float amp_low, freq_low, amp_high, freq_high; } VibrationValue;
static float motor_amp[2][2];   // [motor][low, high]
static PlatMutex vib_lock = PLAT_MUTEX_INIT;

static void vib_handles(Cpu *c) {
    uint32_t *h = arg_p(c, 0);
    int n = (int)arg_x(c, 1);
    uint32_t id = id_arg(c, 2), style = (uint32_t)arg_x(c, 3);
    if (!h || n <= 0 || !(style & style_of(id))) {
        ret_x(c, 0);
        return;
    }
    uint32_t base = id << 8 | (id == NPAD_HANDHELD ? 4u : 3u);
    int count = n < 2 ? n : 2;
    for (int i = 0; i < count; i++) h[i] = base | (uint32_t)i << 16;
    ret_x(c, (uint64_t)count);
}

static void vib_send(const uint32_t *h, const VibrationValue *v, int n) {
    if (!h || !v || n <= 0) return;
    plat_mutex_lock(&vib_lock);
    for (int i = 0; i < n; i++) {
        int motor = h[i] >> 16 & 1;
        motor_amp[motor][0] = v[i].amp_low;
        motor_amp[motor][1] = v[i].amp_high;
    }
    // The pad has a heavy (low) and a light (high) motor; the two Joy-Con motors' low and high bands mix into them.
    float low = fmaxf(motor_amp[0][0], motor_amp[1][0]), high = fmaxf(motor_amp[0][1], motor_amp[1][1]);
    plat_mutex_unlock(&vib_lock);
    input_rumble(low, high, 200);
}

static void vib_init(Cpu *c) {
    static const VibrationValue idle = {0, 160, 0, 320};
    vib_send(arg_p(c, 0), &idle, 1);
}
static void vib_send_one(Cpu *c) { vib_send(arg_p(c, 0), arg_p(c, 1), 1); }
static void vib_send_many(Cpu *c) {
    vib_send(arg_p(c, 0), arg_p(c, 1), (int)arg_x(c, 2));
    ret_x(c, RES_OK);
}
static void vib_permitted(Cpu *c) { ret_x(c, 1); }

HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", state_full_key);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", state_handheld);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", state_joy_dual);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", state_joy_left);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", state_joy_right);
HOST_OVERRIDE("_ZN2nn3hid14InitializeNpadEv", initialize_npad);
HOST_OVERRIDE("_ZN2nn3hid14DisconnectNpadERKj", nothing);
HOST_OVERRIDE("_ZN2nn3hid15GetNpadStyleSetERKj", get_style);
HOST_OVERRIDE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", set_styles);
HOST_OVERRIDE("_ZN2nn3hid24GetSupportedNpadStyleSetEv", get_styles);
HOST_OVERRIDE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", nothing);
HOST_OVERRIDE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", nothing);
HOST_OVERRIDE("_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE", nothing);
HOST_OVERRIDE("_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj", nothing);
HOST_OVERRIDE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj", nothing);
HOST_OVERRIDE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKjNS0_17NpadJoyDeviceTypeE", nothing);
HOST_OVERRIDE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleEPjRKjNS0_17NpadJoyDeviceTypeE", joy_single_out);
HOST_OVERRIDE("_ZN2nn3hid23MergeSingleJoyAsDualJoyERKjS2_", ok);
HOST_OVERRIDE("_ZN2nn3hid18SwapNpadAssignmentERKjS2_", nothing);
HOST_OVERRIDE("_ZN2nn3hid15InitializeMouseEv", nothing);
HOST_OVERRIDE("_ZN2nn3hid18InitializeKeyboardEv", nothing);
HOST_OVERRIDE("_ZN2nn3hid21InitializeTouchScreenEv", nothing);
HOST_OVERRIDE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",
              controller_support);
HOST_OVERRIDE("_ZN2nn3hid24ShowControllerStrapGuideEv", ok);
HOST_OVERRIDE("_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorERKj", controller_color);
HOST_OVERRIDE("_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorES2_RKj", controller_color2);
HOST_OVERRIDE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
              six_handles);
HOST_OVERRIDE("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", nothing);
HOST_OVERRIDE("_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE", nothing);
HOST_OVERRIDE("_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", six_state);
HOST_OVERRIDE("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", six_states_n);
HOST_OVERRIDE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE",
              vib_handles);
HOST_OVERRIDE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", vib_init);
HOST_OVERRIDE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", vib_send_one);
HOST_OVERRIDE("_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi",
              vib_send_many);
HOST_OVERRIDE("_ZN2nn3hid20IsVibrationPermittedEv", vib_permitted);
