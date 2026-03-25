#pragma once

#include <cstddef>

// ─── Session Config Builder ─────────────────────────────────────────────────
//
// Builds the JSON payload for the "session.update" message sent to the
// OpenAI Realtime API after the WebSocket connects.
//
// Includes:
//   - Audio input/output format (PCM 24kHz)
//   - Noise reduction (far_field)
//   - Transcription model
//   - Semantic VAD turn detection
//   - Voice selection
//   - System instructions with radio context
//   - Tool definitions from voice_tools
//
// The caller must provide a buffer large enough for the JSON.
// Returns the number of bytes written (excluding null terminator),
// or -1 if the buffer was too small.

int voice_session_build_update(char *buf, size_t buf_len);

// Recommended minimum buffer size for voice_session_build_update.
constexpr size_t VOICE_SESSION_BUF_SIZE = 4096;

// ─── Configurable Instructions ──────────────────────────────────────────────
//
// Override the default system instructions used in the session.update payload.
// Pass a pointer to a string that remains valid for the lifetime of the session.
// Pass nullptr to revert to the built-in default instructions.

void voice_session_set_instructions(const char *instructions);
