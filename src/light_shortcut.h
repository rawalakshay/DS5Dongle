#pragma once

#include <cstdint>

// Controller combo (D-pad Left + L1 + Triangle) that cycles lightbar_mode.
// light_shortcut_tick() is fed every BT input report from on_bt_data();
// light_shortcut_task() runs in the core-0 main loop for the deferred save.
void light_shortcut_tick(const uint8_t *data, uint16_t len);
void light_shortcut_task();
