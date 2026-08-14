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
        // Push the new mode to the controller right away, mirroring set_config().
        // In host-controlled mode this sends an all-zero (ignored) state packet.
        const SetStateData state{};
        update_state(state);
        save_pending = true;
        last_change_ms = now;
    }
    combo_prev = combo;
}

void light_shortcut_task() {
    if (!save_pending) return;
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_change_ms < SAVE_SETTLE_MS) return;
    save_pending = false;
    config_save();
}
