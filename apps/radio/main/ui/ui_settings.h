#pragma once

#include "lvgl.h"

void ui_settings_init();

// Trigger a rescan and show results on the settings page.
// Can be called from any task — uses display_lock internally.
void ui_settings_rescan();

// Called by ui.cpp when encoder rotates while settings page is active.
void ui_settings_encoder(int steps);

// Called by ui.cpp when tap occurs while settings page is active.
void ui_settings_tap();

// Check if settings page is currently visible.
bool ui_settings_is_active();
