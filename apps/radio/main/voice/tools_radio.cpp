#include "app_config.h"
#include "settings.h"
#include "voice_tools.h"

#include "esp_event.h"
#include "esp_log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

static constexpr const char *TAG = "tools_radio";

// ─── Case-Insensitive Substring Match ───────────────────────────────────────

static bool icontains(const char *haystack, const char *needle) {
  if (!haystack || !needle || !*needle)
    return false;
  for (const char *h = haystack; *h; h++) {
    const char *a = h;
    const char *b = needle;
    while (*a && *b) {
      char ca = (*a >= 'A' && *a <= 'Z') ? (*a + 32) : *a;
      char cb = (*b >= 'A' && *b <= 'Z') ? (*b + 32) : *b;
      if (ca != cb)
        break;
      a++;
      b++;
    }
    if (!*b)
      return true;
  }
  return false;
}

// ─── play_station ───────────────────────────────────────────────────────────

static bool handle_play_station(const char *args, ToolResult *r) {
  char name[64] = {};
  if (!tool_json_get_string(args, "station_name", name, sizeof(name))) {
    r->success = false;
    snprintf(r->output, sizeof(r->output), "Missing station_name parameter.");
    return true;
  }

  int best = -1;
  for (int i = 0; i < RADIO_STATION_COUNT; i++) {
    if (icontains(RADIO_STATIONS[i].name, name)) {
      best = i;
      break;
    }
  }

  if (best < 0) {
    r->success = false;
    size_t pos = snprintf(r->output, sizeof(r->output),
                          "No station matching '%s'. Available: ", name);
    for (int i = 0; i < RADIO_STATION_COUNT && pos < sizeof(r->output) - 20;
         i++) {
      int w = snprintf(r->output + pos, sizeof(r->output) - pos, "%s%s",
                       i > 0 ? ", " : "", RADIO_STATIONS[i].name);
      if (w > 0)
        pos += w;
    }
    return true;
  }

  int32_t idx = best;
  esp_event_post(APP_EVENT, APP_EVENT_STATION_CHANGED, &idx, sizeof(idx), 0);

  r->success = true;
  snprintf(r->output, sizeof(r->output), "Now playing %s.",
           RADIO_STATIONS[best].name);
  ESP_LOGI(TAG, "Switching to station: %s", RADIO_STATIONS[best].name);
  return true;
}

REGISTER_TOOL(
    play_station, "play_station",
    "Switch to a radio station by name. Available stations: "
    "NRK P2, NRK Alltid Nyheter.",
    R"J({"type":"object","properties":{"station_name":{"type":"string","description":"Name of the station to play (case-insensitive partial match)"}},"required":["station_name"]})J",
    handle_play_station);

// ─── play_playlist ──────────────────────────────────────────────────────────

static bool handle_play_playlist(const char *args, ToolResult *r) {
  char user_name[64] = {};
  char playlist_name[64] = {};

  if (!tool_json_get_string(args, "user_name", user_name, sizeof(user_name))) {
    r->success = false;
    snprintf(r->output, sizeof(r->output), "Missing user_name parameter.");
    return true;
  }

  if (!tool_json_get_string(args, "playlist_name", playlist_name,
                            sizeof(playlist_name))) {
    r->success = false;
    snprintf(r->output, sizeof(r->output), "Missing playlist_name parameter.");
    return true;
  }

  // Find user by case-insensitive partial match
  int user_idx = -1;
  for (int i = 0; i < USER_COUNT; i++) {
    if (icontains(USERS[i].name, user_name)) {
      user_idx = i;
      break;
    }
  }

  if (user_idx < 0) {
    r->success = false;
    size_t pos = snprintf(r->output, sizeof(r->output),
                          "No user matching '%s'. Available users: ", user_name);
    for (int i = 0; i < USER_COUNT && pos < sizeof(r->output) - 20; i++) {
      int w = snprintf(r->output + pos, sizeof(r->output) - pos, "%s%s",
                       i > 0 ? ", " : "", USERS[i].name);
      if (w > 0)
        pos += w;
    }
    return true;
  }

  // Find playlist by case-insensitive partial match within user's playlists
  const User &user = USERS[user_idx];
  int pl_idx = -1;
  for (int i = 0; i < user.playlist_count; i++) {
    if (icontains(user.playlists[i].name, playlist_name)) {
      pl_idx = i;
      break;
    }
  }

  if (pl_idx < 0) {
    r->success = false;
    size_t pos =
        snprintf(r->output, sizeof(r->output),
                 "No playlist matching '%s' for %s. Available playlists: ",
                 playlist_name, user.name);
    for (int i = 0; i < user.playlist_count && pos < sizeof(r->output) - 20;
         i++) {
      int w = snprintf(r->output + pos, sizeof(r->output) - pos, "%s%s",
                       i > 0 ? ", " : "", user.playlists[i].name);
      if (w > 0)
        pos += w;
    }
    return true;
  }

  const char *uri = user.playlists[pl_idx].uri;
  esp_event_post(APP_EVENT, 210, (void *)uri, strlen(uri) + 1, 0);

  r->success = true;
  snprintf(r->output, sizeof(r->output), "Now playing %s's playlist: %s.",
           user.name, user.playlists[pl_idx].name);
  ESP_LOGI(TAG, "Playing playlist: %s - %s (%s)", user.name,
           user.playlists[pl_idx].name, uri);
  return true;
}

REGISTER_TOOL(
    play_playlist, "play_playlist",
    "Play a Spotify playlist for a family member. "
    "Available users and playlists: "
    "Sindre (Liked Songs, Chill Vibes, Workout), "
    "Ida (Liked Songs, Focus, Party), "
    "Isak (Liked Songs, Gaming, Bedtime).",
    R"J({"type":"object","properties":{"user_name":{"type":"string","description":"Name of the user (case-insensitive partial match)"},"playlist_name":{"type":"string","description":"Name of the playlist (case-insensitive partial match)"}},"required":["user_name","playlist_name"]})J",
    handle_play_playlist);

// ─── set_volume ─────────────────────────────────────────────────────────────

static bool handle_set_volume(const char *args, ToolResult *r) {
  int level = -1;
  if (!tool_json_get_int(args, "level", &level)) {
    r->success = false;
    snprintf(r->output, sizeof(r->output), "Missing level parameter (0-100).");
    return true;
  }

  level = std::clamp(level, VOLUME_MIN, VOLUME_MAX);
  int32_t vol = level;
  esp_event_post(APP_EVENT, APP_EVENT_VOLUME_CHANGED, &vol, sizeof(vol), 0);

  r->success = true;
  snprintf(r->output, sizeof(r->output), "Volume set to %d%%.", level);
  ESP_LOGI(TAG, "Volume set to %d", level);
  return true;
}

REGISTER_TOOL(
    set_volume, "set_volume", "Set the Sonos speaker volume.",
    R"J({"type":"object","properties":{"level":{"type":"integer","description":"Volume level 0-100"}},"required":["level"]})J",
    handle_set_volume);

// ─── get_now_playing ────────────────────────────────────────────────────────

static bool handle_get_now_playing(const char *, ToolResult *r) {
  int idx = settings_get_station_index();
  const char *station = (idx >= 0 && idx < RADIO_STATION_COUNT)
                            ? RADIO_STATIONS[idx].name
                            : "Unknown";

  r->success = true;
  snprintf(r->output, sizeof(r->output), "Station: %s (index %d of %d).",
           station, idx + 1, RADIO_STATION_COUNT);
  return true;
}

REGISTER_TOOL(get_now_playing, "get_now_playing",
              "Get the currently playing station name, play state, and volume.",
              R"J({"type":"object","properties":{}})J", handle_get_now_playing);
