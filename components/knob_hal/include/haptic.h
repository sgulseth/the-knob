#pragma once

#include <cstdint>

void haptic_init();

// Play a specific DRV2605 Library A waveform effect (1-123).
void haptic_play(uint8_t effect);

// Convenience alias — plays Strong Click (effect 1).
void haptic_buzz();

// ─── Named Effects ──────────────────────────────────────────────────────────
// DRV2605 Library A effect numbers for semantic haptic feedback.

constexpr uint8_t HAPTIC_CLICK      = 1;   // Strong Click — tap confirmations
constexpr uint8_t HAPTIC_TICK       = 10;  // Short Double Click — encoder browse
constexpr uint8_t HAPTIC_MODE       = 17;  // Sharp Click — mode/category switch
constexpr uint8_t HAPTIC_BUMP       = 47;  // Buzz 1 — volume end-stop
constexpr uint8_t HAPTIC_ALERT      = 52;  // Pulsing Strong 1 — timer fired
