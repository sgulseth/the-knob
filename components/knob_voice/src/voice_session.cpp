#include "voice_session.h"
#include "voice_config.h"
#include "voice_tools.h"

#include <cstdio>
#include <cstring>

static constexpr const char *DEFAULT_INSTRUCTIONS =
    "You are a helpful voice assistant built into a Sonos radio controller. "
    "Be concise — answers should be 1-3 sentences. Speak naturally and warmly. "
    "The user is controlling a Sonos speaker playing Norwegian radio stations. "
    "You can set timers, switch stations, adjust volume, and check what's "
    "playing. "
    "When the user asks to play a station, use the play_station tool. "
    "When the user asks about time remaining on a timer, use get_timer_status. "
    "Always confirm actions briefly after executing a tool.";

static const char *s_instructions = nullptr;

void voice_session_set_instructions(const char *instructions) {
  s_instructions = instructions;
}

int voice_session_build_update(char *buf, size_t buf_len) {
  char tools[2048];
  int tools_len = voice_tools_build_json(tools, sizeof(tools));
  if (tools_len < 0)
    return -1;

  const char *instructions = s_instructions ? s_instructions : DEFAULT_INSTRUCTIONS;

  int written =
      snprintf(buf, buf_len,
               "{"
               "\"type\":\"session.update\","
               "\"session\":{"
               "\"model\":\"gpt-realtime-1.5\","
               "\"instructions\":\"%s\","
               "\"audio\":{"
               "\"input\":{"
               "\"format\":{\"type\":\"audio/pcm\",\"rate\":24000},"
               "\"noise_reduction\":{\"type\":\"far_field\"},"
               "\"transcription\":{\"model\":\"gpt-4o-mini-transcribe\"},"
               "\"turn_detection\":{"
               "\"type\":\"semantic_vad\","
               "\"eagerness\":\"high\","
               "\"create_response\":true,"
               "\"interrupt_response\":true"
               "}"
               "},"
               "\"output\":{"
               "\"format\":{\"type\":\"audio/pcm\",\"rate\":24000},"
               "\"voice\":\"%s\""
               "}"
               "},"
               "\"tools\":%s,"
               "\"tool_choice\":\"auto\","
               "\"output_modalities\":[\"audio\"],"
               "\"max_output_tokens\":1024"
               "}"
               "}",
               instructions, OPENAI_VOICE, tools);

  if (written < 0 || static_cast<size_t>(written) >= buf_len)
    return -1;

  return written;
}
