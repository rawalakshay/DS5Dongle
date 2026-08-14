//
// D-pad Left + L1 + Triangle held together cycles lightbar_mode 0 -> 1 -> 2 -> 0.
//

#include "light_shortcut.h"

#include <cstdio>

#include "bt.h"
#include "config.h"
#include "utils.h"
#include "pico/time.h"

// The combo must be held this long before it fires: all three bits can align
// for a report or two while mashing, so a transient overlap must not trigger.
#define COMBO_HOLD_MS 200
// The flash save runs this long after the last mode change, batching rapid
// cycling into one write and keeping the flash stall (core1 parked, brief
// audio blackout) away from the moment of the press.
#define SAVE_SETTLE_MS 10000

static bool combo_prev = false;
static uint32_t combo_since_ms = 0;
static bool fired = false;
static bool save_pending = false;
static uint32_t last_change_ms = 0;

// Rumble confirmation played when the shortcut changes mode. The pulse count
// identifies the mode that was just selected: 1 = battery tiers, 2 =
// host-controlled, 3 = custom RGB. Steps advance in light_shortcut_task.
#define HAPTIC_STRENGTH 0x80
#define HAPTIC_ON_MS    100
#define HAPTIC_GAP_MS   120
#define HAPTIC_RETRY_MS  10
// Separator inserted when a sequence restarts, so the pulses already felt from
// the interrupted count read as their own train instead of merging into the
// new one - with an equal gap the user would just feel a longer burst.
#define HAPTIC_RESTART_GAP_MS 400
static int haptic_pulses_left = 0;          // pulses still to start (0 = none)
static bool haptic_on = false;              // motors currently commanded on
static bool haptic_release_pending = false; // owe a stop/release packet
static bool haptic_long_gap = false;        // next gap separates two sequences
static uint32_t haptic_step_ms = 0;         // next pulse step
static uint32_t haptic_release_ms = 0;      // next release attempt

// One confirmation packet. use_rumble selects the rumble-emulation actuator
// path; the packet that closes a sequence clears it so the controller returns
// to the haptic audio channel this firmware streams (see audio.cpp), which
// that bit otherwise keeps overridden.
static bool send_rumble(const uint8_t strength, const bool use_rumble) {
    SetStateData state{};
    state.EnableRumbleEmulation = 1;
    state.UseRumbleNotHaptics = use_rumble;
    state.RumbleEmulationLeft = strength;
    state.RumbleEmulationRight = strength;
    return update_state(state);
}

// Wrap-safe deadline test: to_ms_since_boot() wraps every ~49.7 days.
static bool time_reached(const uint32_t now, const uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

void light_shortcut_tick(const uint8_t *data, uint16_t len) {
    if (len < 10) return;
    if (!get_config().lightbar_shortcut_enabled) return;

    // Byte 7: low nibble = D-pad hat (6 = West/Left), bit 7 = Triangle.
    // Byte 8: bit 0 = L1.
    const bool combo = (data[7] & 0x0F) == 6 && (data[7] & 0x80) && (data[8] & 0x01);
    const uint32_t now = to_ms_since_boot(get_absolute_time());

    if (combo && !combo_prev) {
        combo_since_ms = now;
        fired = false;
    }
    if (combo && !fired && now - combo_since_ms >= COMBO_HOLD_MS) {
        fired = true; // once per hold; release re-arms
        Config_body &cfg = get_config();
        cfg.lightbar_mode = (cfg.lightbar_mode + 1) % 3;
        printf("[LIGHT] Shortcut: lightbar_mode -> %u\n", cfg.lightbar_mode);
        // Repaint for the new mode on the next tick. A plain update_state()
        // here would be a no-op in host-controlled mode (apply_lightbar leaves
        // that mode alone), leaving the old mode's color latched.
        lightbar_note_mode_changed();
        save_pending = true;
        last_change_ms = now;
        // Confirm the switch with mode+1 rumble pulses (1/2/3) so the user
        // can tell which mode they landed on without looking at the lightbar.
        // Restarting mid-sequence: the pulses already felt cannot be taken
        // back, so end that train promptly and separate it from the new count.
        const bool restarting = haptic_on || haptic_pulses_left > 0;
        haptic_pulses_left = cfg.lightbar_mode + 1;
        if (haptic_on) {
            haptic_step_ms = now;      // close the in-flight pulse now
            haptic_long_gap = true;    // ...then separate before counting
        } else {
            haptic_step_ms = restarting ? now + HAPTIC_RESTART_GAP_MS : now;
        }
        // This sequence ends with its own release packet, so drop any release
        // still owed from an earlier one - firing it now would cut the motors
        // mid-pulse and make the count unreadable.
        haptic_release_pending = false;
    }
    combo_prev = combo;
}

void light_shortcut_task() {
    const uint32_t now = to_ms_since_boot(get_absolute_time());

    // A torn-down sequence owes the controller a stop/release: the motors may
    // be commanded on, and every non-final off packet leaves the rumble path
    // selected. The reconnect state packet clears neither. Backs off on a full
    // send FIFO rather than retrying (and logging) every loop iteration.
    if (haptic_release_pending && bt_is_connected() && time_reached(now, haptic_release_ms)) {
        if (send_rumble(0, false)) {
            haptic_release_pending = false;
        } else {
            haptic_release_ms = now + HAPTIC_RETRY_MS;
        }
    }

    // Pulse-count rumble confirmation. The off steps must land even when the
    // send FIFO is momentarily full, so a failed send retries shortly instead
    // of advancing - otherwise the rumble could stay stuck on.
    if (haptic_pulses_left > 0 || haptic_on) {
        if (!bt_is_connected()) {
            // Owed on any teardown, not just mid-pulse: a drop during a gap
            // still leaves UseRumbleNotHaptics set from the last off packet.
            haptic_release_pending = true;
            haptic_release_ms = now;
            haptic_pulses_left = 0;
            haptic_on = false;
        } else if (time_reached(now, haptic_step_ms)) {
            if (!haptic_on) {
                // Count the pulse as it starts, so a restarted sequence cannot
                // lose one to the off step that closes the previous pulse.
                if (send_rumble(HAPTIC_STRENGTH, true)) {
                    haptic_on = true;
                    haptic_pulses_left--;
                    haptic_step_ms = now + HAPTIC_ON_MS;
                } else {
                    haptic_step_ms = now + HAPTIC_RETRY_MS;
                }
            } else {
                // The packet that ends the last pulse also releases the
                // rumble-emulation path back to streamed haptics.
                const bool last = haptic_pulses_left == 0;
                if (send_rumble(0, !last)) {
                    haptic_on = false;
                    haptic_step_ms = now + (haptic_long_gap ? HAPTIC_RESTART_GAP_MS
                                                            : HAPTIC_GAP_MS);
                    haptic_long_gap = false;
                    if (last) {
                        // bt_write() reports "queued", not "delivered". If this
                        // packet is dropped downstream the controller stays on
                        // the rumble path with streamed haptics muted, so send
                        // one confirming release instead of trusting one packet.
                        haptic_release_pending = true;
                        haptic_release_ms = now + HAPTIC_GAP_MS;
                    }
                } else {
                    haptic_step_ms = now + HAPTIC_RETRY_MS;
                }
            }
        }
    }

    if (!save_pending) return;
    if (now - last_change_ms < SAVE_SETTLE_MS) return;
    save_pending = false;
    config_save();
}
