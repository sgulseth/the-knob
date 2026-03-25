#pragma once

#include "hal_pins.h"
#include "knob_events.h"
#include "sonos_config.h"
#include "voice_config.h"
#include "timer_events.h"

// ─── Enums ──────────────────────────────────────────────────────────────────

enum class AppScreen {
  ModeSelect,
  RadioBrowse,
  UserSelect,
  PlaylistSelect,
  NowPlaying,
};

enum class JukeboxMode {
  Radio,
  Playlist,
};

// ─── Radio Stations ─────────────────────────────────────────────────────────

struct Station {
  const char *name;
  const char *url;
  uint32_t color;
};

constexpr Station RADIO_STATIONS[] = {
    {"NRK P2",
     "https://cdn0-47115-liveicecast0.dna.contentdelivery.net/p2_aac_h",
     0x280514},
    {"NRK Alltid Nyheter",
     "https://cdn0-47115-liveicecast0.dna.contentdelivery.net/nyheter_aac_h",
     0x051428},
};

constexpr int RADIO_STATION_COUNT =
    sizeof(RADIO_STATIONS) / sizeof(RADIO_STATIONS[0]);

// ─── Playlists & Users ─────────────────────────────────────────────────────

struct Playlist {
  const char *name;
  const char *uri;
};

struct User {
  const char *name;
  uint32_t color;
  const Playlist *playlists;
  int playlist_count;
};

constexpr Playlist SINDRE_PLAYLISTS[] = {
    {"Liked Songs", "spotify:playlist:PLACEHOLDER_SINDRE_1"},
    {"Chill Vibes", "spotify:playlist:PLACEHOLDER_SINDRE_2"},
    {"Workout", "spotify:playlist:PLACEHOLDER_SINDRE_3"},
};

constexpr Playlist IDA_PLAYLISTS[] = {
    {"Liked Songs", "spotify:playlist:PLACEHOLDER_IDA_1"},
    {"Focus", "spotify:playlist:PLACEHOLDER_IDA_2"},
    {"Party", "spotify:playlist:PLACEHOLDER_IDA_3"},
};

constexpr Playlist ISAK_PLAYLISTS[] = {
    {"Liked Songs", "spotify:playlist:PLACEHOLDER_ISAK_1"},
    {"Gaming", "spotify:playlist:PLACEHOLDER_ISAK_2"},
    {"Bedtime", "spotify:playlist:PLACEHOLDER_ISAK_3"},
};

constexpr User USERS[] = {
    {"Sindre", 0x1DB954, SINDRE_PLAYLISTS,
     sizeof(SINDRE_PLAYLISTS) / sizeof(SINDRE_PLAYLISTS[0])},
    {"Ida", 0xE91E63, IDA_PLAYLISTS,
     sizeof(IDA_PLAYLISTS) / sizeof(IDA_PLAYLISTS[0])},
    {"Isak", 0x2196F3, ISAK_PLAYLISTS,
     sizeof(ISAK_PLAYLISTS) / sizeof(ISAK_PLAYLISTS[0])},
};

constexpr int USER_COUNT = sizeof(USERS) / sizeof(USERS[0]);

// ─── Volume ─────────────────────────────────────────────────────────────────

constexpr int VOLUME_CAP_ON_SOURCE_CHANGE = 60;

// ─── Voice Assistant ────────────────────────────────────────────────────────

constexpr const char *VOICE_INSTRUCTIONS =
    "You are the voice assistant for a family jukebox. "
    "The jukebox has two modes: Radio and Playlist. "
    "In Radio mode there are two stations: NRK P2 (culture and debate) "
    "and NRK Alltid Nyheter (24/7 news). "
    "In Playlist mode there are three family members: "
    "Sindre, Ida, and Isak — each with their own Spotify playlists. "
    "Help the user pick a mode, station, or playlist. "
    "Keep responses short and friendly.";

// ─── Task Config ────────────────────────────────────────────────────────────

constexpr int UI_TASK_STACK = 8192;
constexpr int UI_TASK_PRIO = 5;
constexpr int UI_TASK_CORE = 0;
