// SDK functions of the Switch 2 (SDK 20, "Ounce") beyond the Switch 1 SDK's: they reach services
// (am, gs) the host does not run, so the SDK's own code would use a null session.
#include <string.h>

#include "host.h"
#include "ov.h"
#include "platform/platform.h"

// ---- nn::oe
// void GetDisplayVersion(DisplayVersion *): the 16-byte version string of the running build
static void ov_display_version(Cpu *c) {
    char *v = arg_p(c, 0);
    if (!v) return;
    memset(v, 0, 16);
    memcpy(v, "1.1.1", 5);
}

// void GetPseudoDeviceId(util::Uuid *): a fixed value, the same on every run (the game may key local data to it)
static void ov_pseudo_device_id(Cpu *c) {
    static const uint8_t id[16] = {'m', 't', 'f', '-', 'r', 'e', 'c', 'o', 'm', 'p', '-', 'h', 'o', 's', 't', '1'};
    uint8_t *out = arg_p(c, 0);
    if (out) memcpy(out, id, 16);
}

// int GetCurrentGraphicsScale(): 0 handheld, 1 console (Switch 2, through am); it follows the operation mode
static void ov_graphics_scale(Cpu *c) { ret_x(c, ov_handheld() ? 0 : 1); }

// void SetGraphicsScaleChangedNotificationEnabled(bool): the scale changes only with the operation mode
static void ov_graphics_scale_notify(Cpu *c) { (void)c; }

// void GetDefaultDisplayResolution(int *w, int *h): 1920x1080 docked, 1280x720 handheld
static void ov_default_resolution(Cpu *c) {
    int *w = arg_p(c, 0), *h = arg_p(c, 1), docked = !ov_handheld();
    if (w) *w = docked ? 1920 : 1280;
    if (h) *h = docked ? 1080 : 720;
}

// void GetDefaultDisplayResolutionChangeEvent(os::SystemEvent *): bound, never signalled; a mode change comes as the
// operation mode notification
static void ov_default_resolution_event(Cpu *c) { kern_bind_system_event(c, arg_x(c, 0), 0); }

// os::SystemEventType *GetNotificationMessageEvent(): an auto-clear event in guest-visible kernel memory, signalled
// whenever ov_system.c's queue receives a message (oe_message_event_signal), and once at binding when it holds some.
#define SYSTEM_EVENT_BYTES 0x100
static uint64_t message_event;
static PlatMutex event_lock = PLAT_MUTEX_INIT;

static void ov_message_event(Cpu *c) {
    plat_mutex_lock(&event_lock);
    if (!message_event) {
        message_event = (uint64_t)(uintptr_t)mem_kernel_alloc(SYSTEM_EVENT_BYTES, 16);
        kern_bind_system_event(c, message_event, 1);
    }
    uint64_t ev = message_event;
    plat_mutex_unlock(&event_lock);
    if (oe_pending_messages()) kern_signal_system_event(ev);
    ret_x(c, ev);
}

void oe_message_event_signal(void) {
    uint64_t ev = __atomic_load_n(&message_event, __ATOMIC_ACQUIRE);
    if (ev) kern_signal_system_event(ev);
}

// Exit request critical section: the host ends the process itself (window close), so there is nothing to hold off
static void ov_exit_section(Cpu *c) { (void)c; }

// void SendServerMaintenanceOverlayNotification(PosixTime, PosixTime): no system overlay on the host
static void ov_maintenance_overlay(Cpu *c) { (void)c; }

HOST_OVERRIDE("_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", ov_display_version);
HOST_OVERRIDE("_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", ov_pseudo_device_id);
HOST_OVERRIDE("_ZN2nn2oe23GetCurrentGraphicsScaleEv", ov_graphics_scale);
HOST_OVERRIDE("_ZN2nn2oe42SetGraphicsScaleChangedNotificationEnabledEb", ov_graphics_scale_notify);
HOST_OVERRIDE("_ZN2nn2oe27GetDefaultDisplayResolutionEPiS1_", ov_default_resolution);
HOST_OVERRIDE("_ZN2nn2oe38GetDefaultDisplayResolutionChangeEventEPNS_2os11SystemEventE", ov_default_resolution_event);
HOST_OVERRIDE("_ZN2nn2oe27GetNotificationMessageEventEv", ov_message_event);
HOST_OVERRIDE("_ZN2nn2oe31EnterExitRequestHandlingSectionEv", ov_exit_section);
HOST_OVERRIDE("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", ov_exit_section);
HOST_OVERRIDE("_ZN2nn2oe40SendServerMaintenanceOverlayNotificationENS_4time9PosixTimeES2_", ov_maintenance_overlay);
