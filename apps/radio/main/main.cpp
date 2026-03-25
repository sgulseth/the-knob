#include "app_config.h"
#include "encoder.h"
#include "haptic.h"
#include "wifi_manager.h"
#include "discovery.h"
#include "sonos.h"
#include "settings.h"
#include "timer.h"

#include "ui/ui.h"
#include "ui/ui_timer.h"
#include "ui/ui_voice.h"
#include "voice_task.h"
#include "voice_tools.h"
#include "voice_session.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <cstring>
#include <ctime>

static constexpr const char *TAG = "main";

// ─── App-level Events (after Sonos events at 200) ──────────────────────────

enum : int32_t {
  APP_EVENT_PLAYLIST_PLAY_REQUESTED = 210,
  APP_EVENT_MODE_CHANGED = 211,
};

ESP_EVENT_DEFINE_BASE(APP_EVENT);

static int s_volume;
static int s_station_index;
static esp_timer_handle_t s_timer_tick_handle;

static void on_station_changed(void *, esp_event_base_t, int32_t, void *data) {
  auto index = *static_cast<int32_t *>(data);
  if (index < 0 || index >= RADIO_STATION_COUNT)
    return;
  s_station_index = index;
  settings_set_station_index(s_station_index);
  // Note: don't play here — on_play_requested handles playback.
  // confirm_browse() posts both STATION_CHANGED and PLAY_REQUESTED.
}

static void on_volume_changed(void *, esp_event_base_t, int32_t, void *data) {
  auto vol = *static_cast<int32_t *>(data);
  s_volume = vol;
  settings_set_volume(vol);
}

static void on_sonos_state(void *, esp_event_base_t, int32_t, void *data) {
  auto *state = static_cast<SonosState *>(data);
  ui_set_play_state(state->play_state);
  if (state->volume != s_volume) {
    s_volume = state->volume;
    ui_set_volume(s_volume);
  }
  if (state->station_index >= 0) {
    ui_set_station(state->station_index);
    ui_set_media_info(nullptr);
  } else if (state->media.has_media) {
    ui_set_media_info(&state->media);
  }
}

static void on_timer_tick(void *) { timer_tick(); }

static void on_timer_started(void *, esp_event_base_t, int32_t, void *data) {
  auto total = *static_cast<int32_t *>(data);
  char label[64];
  timer_get_label(label, sizeof(label));
  ESP_LOGI(TAG, "Timer started: %ds '%s'", (int)total, label);
  ui_timer_show(total, label);
}

static void on_timer_fired(void *, esp_event_base_t, int32_t, void *data) {
  auto *label = static_cast<const char *>(data);
  ESP_LOGI(TAG, "Timer fired: %s", label ? label : "(none)");
  haptic_buzz();
}

// ─── Voice Mode Events ──────────────────────────────────────────────────────

static void on_voice_activate(void *, esp_event_base_t, int32_t, void *) {
  ESP_LOGI(TAG, "Voice mode activated");
  voice_task_start();
}

static void on_voice_deactivate(void *, esp_event_base_t, int32_t, void *) {
  ESP_LOGI(TAG, "Voice mode deactivated");
  voice_task_stop();
}

static void on_voice_state(void *, esp_event_base_t, int32_t, void *data) {
  auto state = *static_cast<VoiceState *>(data);
  if (state == VoiceState::Inactive) {
    voice_ui_exit();
  } else {
    voice_ui_set_state(state);
  }
}

static void on_voice_transcript(void *, esp_event_base_t, int32_t, void *data) {
  auto *text = static_cast<const char *>(data);
  // Determine if this is user or AI text based on current voice state
  // (Thinking = user just spoke, Speaking = AI responding)
  // For now, show all transcripts as AI text (non-dimmed)
  voice_ui_set_transcript(text, false);
}

static void discover_and_connect_task(void *) {
  ui_show_scanning();

  DiscoveryResult result = {};
  discovery_scan(&result);

  if (result.count == 1) {
    auto &speaker = result.speakers[0];
    ESP_LOGI(TAG, "Single speaker found — auto-selecting: %s", speaker.name);
    sonos_set_speaker(speaker.ip, speaker.port);
    settings_set_speaker_name(speaker.name);
    ui_set_speaker_name(speaker.name);
    sonos_start();
  } else if (result.count > 1) {
    ui_show_speaker_picker(&result);
  } else {
    ESP_LOGW(TAG, "No speakers found — showing picker with empty state");
    ui_show_speaker_picker(&result);
  }

  vTaskDelete(nullptr);
}

static void start_with_saved_speaker() {
  char ip[40] = {};
  char name[64] = {};
  settings_get_speaker_ip(ip, sizeof(ip));
  settings_get_speaker_name(name, sizeof(name));

  ESP_LOGI(TAG, "Reconnecting to saved speaker: %s (%s)", name, ip);
  sonos_set_speaker(ip);
  ui_set_speaker_name(name[0] ? name : ip);
  sonos_start();
}

static void on_wifi_connected(void *, esp_event_base_t, int32_t, void *) {
  ESP_LOGI(TAG, "WiFi connected");
  ui_set_wifi_status(true);

  // Set timezone and start NTP sync (idempotent — safe on reconnect)
  setenv("TZ", CONFIG_RADIO_TIMEZONE, 1);
  tzset();
  static bool sntp_started = false;
  if (!sntp_started) {
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_cfg);
    sntp_started = true;
    ESP_LOGI(TAG, "SNTP started, TZ=%s", CONFIG_RADIO_TIMEZONE);
  }

  if (settings_has_speaker()) {
    start_with_saved_speaker();
  } else if (strlen(CONFIG_RADIO_SONOS_SPEAKER_IP) > 0 &&
             strcmp(CONFIG_RADIO_SONOS_SPEAKER_IP, "192.168.1.100") != 0) {
    // Use .env-configured speaker IP directly (skip discovery)
    ESP_LOGI(TAG, "Using configured speaker: %s",
             CONFIG_RADIO_SONOS_SPEAKER_IP);
    sonos_set_speaker(CONFIG_RADIO_SONOS_SPEAKER_IP);
    sonos_start();

    // Resolve speaker name from device description
    char speaker_name[64] = {};
    if (discovery_get_speaker_name(CONFIG_RADIO_SONOS_SPEAKER_IP, 1400,
                                   speaker_name, sizeof(speaker_name)) &&
        speaker_name[0]) {
      ESP_LOGI(TAG, "Speaker name: %s", speaker_name);
    } else {
      strncpy(speaker_name, CONFIG_RADIO_SONOS_SPEAKER_IP,
              sizeof(speaker_name) - 1);
    }
    settings_set_speaker_name(speaker_name);
    ui_set_speaker_name(speaker_name);
  } else {
    xTaskCreatePinnedToCore(discover_and_connect_task, "discover", 10240,
                            nullptr, NET_TASK_PRIO, nullptr, NET_TASK_CORE);
  }
}

static void on_wifi_disconnected(void *, esp_event_base_t, int32_t, void *) {
  ESP_LOGW(TAG, "WiFi disconnected");
  ui_set_wifi_status(false);
  sonos_stop();
}

static void on_play_requested(void *, esp_event_base_t, int32_t, void *) {
  // Cap volume on source change to avoid blasting audio
  if (s_volume > VOLUME_CAP_ON_SOURCE_CHANGE) {
    ESP_LOGI(TAG, "Volume %d exceeds cap %d — clamping", s_volume,
             VOLUME_CAP_ON_SOURCE_CHANGE);
    s_volume = VOLUME_CAP_ON_SOURCE_CHANGE;
    sonos_set_volume(s_volume);
    ui_set_volume(s_volume);
  }

  ESP_LOGI(TAG, "Play requested — station: %s",
           RADIO_STATIONS[s_station_index].name);
  sonos_play_uri(RADIO_STATIONS[s_station_index].url);
}

static void on_stop_requested(void *, esp_event_base_t, int32_t, void *) {
  ESP_LOGI(TAG, "Stop requested");
  sonos_stop_playback();
}

// ─── Playlist Events ────────────────────────────────────────────────────────

static void on_playlist_play_requested(void *, esp_event_base_t, int32_t,
                                       void *data) {
  auto *uri = static_cast<const char *>(data);
  ESP_LOGI(TAG, "Playlist play requested — uri: %s", uri);

  // Cap volume on source change to avoid blasting audio
  if (s_volume > VOLUME_CAP_ON_SOURCE_CHANGE) {
    ESP_LOGI(TAG, "Volume %d exceeds cap %d — clamping", s_volume,
             VOLUME_CAP_ON_SOURCE_CHANGE);
    s_volume = VOLUME_CAP_ON_SOURCE_CHANGE;
    sonos_set_volume(s_volume);
    ui_set_volume(s_volume);
  }

  sonos_play_uri(uri);
}

static void init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

static void register_events() {
  esp_event_loop_create_default();

  esp_event_handler_register(APP_EVENT, APP_EVENT_STATION_CHANGED,
                             on_station_changed, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_VOLUME_CHANGED,
                             on_volume_changed, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_PLAY_REQUESTED,
                             on_play_requested, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_STOP_REQUESTED,
                             on_stop_requested, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_WIFI_CONNECTED,
                             on_wifi_connected, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_WIFI_DISCONNECTED,
                             on_wifi_disconnected, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_SONOS_STATE_UPDATE,
                             on_sonos_state, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_TIMER_STARTED,
                             on_timer_started, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_TIMER_FIRED, on_timer_fired,
                             nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_VOICE_ACTIVATE,
                             on_voice_activate, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_VOICE_DEACTIVATE,
                             on_voice_deactivate, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_VOICE_STATE, on_voice_state,
                             nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_VOICE_TRANSCRIPT,
                             on_voice_transcript, nullptr);
  esp_event_handler_register(APP_EVENT, APP_EVENT_PLAYLIST_PLAY_REQUESTED,
                             on_playlist_play_requested, nullptr);
}

static void start_timer_tick() {
  esp_timer_create_args_t args = {};
  args.callback = on_timer_tick;
  args.name = "voice_timer";
  ESP_ERROR_CHECK(esp_timer_create(&args, &s_timer_tick_handle));
  ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer_tick_handle, 1'000'000));
}

extern "C" void app_main() {
  ESP_LOGI(TAG, "Sonos Jukebox starting");

  init_nvs();
  settings_init();

  s_volume = settings_get_volume();
  s_station_index = settings_get_station_index();

  register_events();
  ui_init();
  haptic_init();
  encoder_init();
  discovery_init();
  sonos_init();

  // Register station URLs for URI matching
  static const char *station_urls[RADIO_STATION_COUNT];
  for (int i = 0; i < RADIO_STATION_COUNT; i++)
    station_urls[i] = RADIO_STATIONS[i].url;
  sonos_set_stations(station_urls, RADIO_STATION_COUNT);

  timer_init();
  ui_timer_init();
  voice_tools_init();
  voice_task_init();
  voice_session_set_instructions(VOICE_INSTRUCTIONS);
  start_timer_tick();
  wifi_manager_init();

  ESP_LOGI(TAG, "Init complete — station: %s, volume: %d",
           RADIO_STATIONS[s_station_index].name, s_volume);
}
