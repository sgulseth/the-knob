#include "sonos.h"
#include "sonos_config.h"
#include "knob_events.h"
#include "settings.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

static constexpr const char *TAG = "sonos";

static constexpr const char *AV_TRANSPORT_PATH =
    "/MediaRenderer/AVTransport/Control";
static constexpr const char *RENDERING_CONTROL_PATH =
    "/MediaRenderer/RenderingControl/Control";

#define AV_TRANSPORT_NS "urn:schemas-upnp-org:service:AVTransport:1"
#define RENDERING_CONTROL_NS "urn:schemas-upnp-org:service:RenderingControl:1"

// ─── SOAP Templates ─────────────────────────────────────────────────────────

static constexpr const char *SOAP_ENVELOPE_FMT =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
    " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
    "<s:Body>%s</s:Body>"
    "</s:Envelope>";

static constexpr const char *SET_URI_FMT =
    "<u:SetAVTransportURI xmlns:u=\"" AV_TRANSPORT_NS "\">"
    "<InstanceID>0</InstanceID>"
    "<CurrentURI>%s</CurrentURI>"
    "<CurrentURIMetaData></CurrentURIMetaData>"
    "</u:SetAVTransportURI>";

static constexpr const char *PLAY_BODY =
    "<u:Play xmlns:u=\"" AV_TRANSPORT_NS "\">"
    "<InstanceID>0</InstanceID>"
    "<Speed>1</Speed>"
    "</u:Play>";

static constexpr const char *STOP_BODY =
    "<u:Stop xmlns:u=\"" AV_TRANSPORT_NS "\">"
    "<InstanceID>0</InstanceID>"
    "</u:Stop>";

static constexpr const char *GET_TRANSPORT_INFO_BODY =
    "<u:GetTransportInfo xmlns:u=\"" AV_TRANSPORT_NS "\">"
    "<InstanceID>0</InstanceID>"
    "</u:GetTransportInfo>";

static constexpr const char *GET_VOLUME_BODY =
    "<u:GetVolume xmlns:u=\"" RENDERING_CONTROL_NS "\">"
    "<InstanceID>0</InstanceID>"
    "<Channel>Master</Channel>"
    "</u:GetVolume>";

static constexpr const char *SET_VOLUME_FMT =
    "<u:SetVolume xmlns:u=\"" RENDERING_CONTROL_NS "\">"
    "<InstanceID>0</InstanceID>"
    "<Channel>Master</Channel>"
    "<DesiredVolume>%d</DesiredVolume>"
    "</u:SetVolume>";

static constexpr const char *GET_POSITION_INFO_BODY =
    "<u:GetPositionInfo xmlns:u=\"" AV_TRANSPORT_NS "\">"
    "<InstanceID>0</InstanceID>"
    "</u:GetPositionInfo>";

static constexpr const char *PAUSE_BODY =
    "<u:Pause xmlns:u=\"" AV_TRANSPORT_NS "\">"
    "<InstanceID>0</InstanceID>"
    "</u:Pause>";

static constexpr const char *PREVIOUS_BODY =
    "<u:Previous xmlns:u=\"" AV_TRANSPORT_NS "\">"
    "<InstanceID>0</InstanceID>"
    "</u:Previous>";

static constexpr const char *NEXT_BODY =
    "<u:Next xmlns:u=\"" AV_TRANSPORT_NS "\">"
    "<InstanceID>0</InstanceID>"
    "</u:Next>";

// ─── Command Queue ──────────────────────────────────────────────────────────

enum class CmdType : uint8_t {
  PlayUri,
  Play,
  Pause,
  Stop,
  SetVolume,
  Previous,
  Next
};

struct Command {
  CmdType type;
  union {
    char uri[256];
    int volume;
  };
};

static QueueHandle_t s_cmd_queue;
static TaskHandle_t s_task;
static volatile bool s_running;
static char s_speaker_ip[40];
static int s_speaker_port = SONOS_PORT;

// ─── Registered Station URLs (set by app via sonos_set_stations) ────────────

static const char *const *s_station_urls = nullptr;
static int s_station_count = 0;

void sonos_set_stations(const char *const *urls, int count) {
  s_station_urls = urls;
  s_station_count = count;
}

// ─── HTTP Helpers ───────────────────────────────────────────────────────────

struct Response {
  char *data;
  int len;
  int capacity;
};

static esp_err_t on_http_event(esp_http_client_event_t *evt) {
  auto *resp = static_cast<Response *>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_DATA && resp && evt->data_len > 0) {
    int needed = resp->len + evt->data_len;
    if (needed < resp->capacity) {
      memcpy(resp->data + resp->len, evt->data, evt->data_len);
      resp->len += evt->data_len;
      resp->data[resp->len] = '\0';
    }
  }
  return ESP_OK;
}

static bool soap_request(const char *path, const char *action_name,
                         const char *ns, const char *body, Response *resp) {
  char url[80];
  snprintf(url, sizeof(url), "http://%s:%d%s", s_speaker_ip, s_speaker_port,
           path);

  char soap_action[128];
  snprintf(soap_action, sizeof(soap_action), "\"%s#%s\"", ns, action_name);

  char envelope[1024];
  int envelope_len =
      snprintf(envelope, sizeof(envelope), SOAP_ENVELOPE_FMT, body);
  if (envelope_len < 0 || envelope_len >= static_cast<int>(sizeof(envelope))) {
    ESP_LOGE(TAG, "SOAP envelope overflow");
    return false;
  }

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = SONOS_HTTP_TIMEOUT_MS;
  cfg.event_handler = on_http_event;
  cfg.user_data = resp;

  auto *client = esp_http_client_init(&cfg);
  if (!client)
    return false;

  esp_http_client_set_header(client, "Content-Type",
                             "text/xml; charset=\"utf-8\"");
  esp_http_client_set_header(client, "SOAPAction", soap_action);
  esp_http_client_set_post_field(client, envelope, envelope_len);

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "%s failed: %s", action_name, esp_err_to_name(err));
    return false;
  }
  if (status != 200) {
    ESP_LOGW(TAG, "%s HTTP %d — response: %.300s", action_name, status,
             (resp && resp->len > 0) ? resp->data : "(no body)");
    return false;
  }
  return true;
}

static bool soap_fire(const char *path, const char *action_name, const char *ns,
                      const char *body) {
  return soap_request(path, action_name, ns, body, nullptr);
}

// ─── XML Parsing (minimal — no allocator, no library) ───────────────────────

static bool xml_extract(const char *xml, const char *tag, char *out,
                        size_t out_len) {
  char open[64], close[64];
  snprintf(open, sizeof(open), "<%s>", tag);
  snprintf(close, sizeof(close), "</%s>", tag);

  const char *start = strstr(xml, open);
  if (!start)
    return false;
  start += strlen(open);

  const char *end = strstr(start, close);
  if (!end)
    return false;

  size_t len = std::min(static_cast<size_t>(end - start), out_len - 1);
  memcpy(out, start, len);
  out[len] = '\0';
  return true;
}

// Unescape XML entities in-place: &lt; &gt; &amp; &quot; &apos;
static void xml_unescape(char *s) {
  char *r = s, *w = s;
  while (*r) {
    if (*r == '&') {
      if (strncmp(r, "&lt;", 4) == 0) {
        *w++ = '<';
        r += 4;
      } else if (strncmp(r, "&gt;", 4) == 0) {
        *w++ = '>';
        r += 4;
      } else if (strncmp(r, "&amp;", 5) == 0) {
        *w++ = '&';
        r += 5;
      } else if (strncmp(r, "&quot;", 6) == 0) {
        *w++ = '"';
        r += 6;
      } else if (strncmp(r, "&apos;", 6) == 0) {
        *w++ = '\'';
        r += 6;
      } else {
        *w++ = *r++;
      }
    } else {
      *w++ = *r++;
    }
  }
  *w = '\0';
}

// Detect media source from a Sonos TrackURI
static void detect_source(const char *uri, char *out, size_t out_len) {
  if (!uri || !uri[0]) {
    out[0] = '\0';
    return;
  }
  if (strstr(uri, ",spotify:") || strstr(uri, "spotify"))
    strncpy(out, "Spotify", out_len - 1);
  else if (strstr(uri, ",airplay:"))
    strncpy(out, "AirPlay", out_len - 1);
  else if (strncmp(uri, "x-sonos-htastream:", 18) == 0)
    strncpy(out, "TV", out_len - 1);
  else if (strncmp(uri, "x-rincon-stream:", 16) == 0)
    strncpy(out, "Line-In", out_len - 1);
  else if (strncmp(uri, "x-rincon-mp3radio:", 18) == 0 ||
           strncmp(uri, "x-sonosapi-stream:", 18) == 0 ||
           strncmp(uri, "x-sonosapi-radio:", 17) == 0 ||
           strncmp(uri, "aac:", 4) == 0 || strncmp(uri, "hls-radio:", 10) == 0)
    strncpy(out, "Radio", out_len - 1);
  else if (strstr(uri, "apple") || strstr(uri, "music.apple"))
    strncpy(out, "Apple Music", out_len - 1);
  else
    strncpy(out, "Media", out_len - 1);
  out[out_len - 1] = '\0';
}

// Parse DIDL-Lite metadata to extract title and artist
static void parse_media_info(const char *raw_metadata, const char *track_uri,
                             MediaInfo *info) {
  memset(info, 0, sizeof(*info));
  if (!raw_metadata || !raw_metadata[0] ||
      strcmp(raw_metadata, "NOT_IMPLEMENTED") == 0)
    return;

  // The metadata is entity-encoded DIDL-Lite XML — unescape into a work buffer
  static char didl[2048];
  strncpy(didl, raw_metadata, sizeof(didl) - 1);
  didl[sizeof(didl) - 1] = '\0';
  xml_unescape(didl);

  // Extract dc:title
  if (xml_extract(didl, "dc:title", info->title, sizeof(info->title))) {
    xml_unescape(info->title);
    // Avoid using URIs as the title
    if (strstr(info->title, "://"))
      info->title[0] = '\0';
  }

  // Extract dc:creator (primary artist field)
  if (xml_extract(didl, "dc:creator", info->artist, sizeof(info->artist)))
    xml_unescape(info->artist);

  // For radio: if no artist, try r:streamContent
  if (!info->artist[0]) {
    char stream[256] = {};
    if (xml_extract(didl, "r:streamContent", stream, sizeof(stream)) &&
        stream[0]) {
      // Format: "TYPE=SNG|TITLE xxx|ARTIST yyy" or "Artist - Title"
      const char *artist_tag = strstr(stream, "ARTIST ");
      const char *title_tag = strstr(stream, "TITLE ");
      if (artist_tag) {
        artist_tag += 7;
        const char *end = strchr(artist_tag, '|');
        size_t len =
            end ? static_cast<size_t>(end - artist_tag) : strlen(artist_tag);
        len = std::min(len, sizeof(info->artist) - 1);
        memcpy(info->artist, artist_tag, len);
        info->artist[len] = '\0';
        if (title_tag && !info->title[0]) {
          title_tag += 6;
          end = strchr(title_tag, '|');
          len = end ? static_cast<size_t>(end - title_tag) : strlen(title_tag);
          len = std::min(len, sizeof(info->title) - 1);
          memcpy(info->title, title_tag, len);
          info->title[len] = '\0';
        }
      } else {
        const char *sep = strstr(stream, " - ");
        if (sep) {
          size_t alen = std::min(static_cast<size_t>(sep - stream),
                                 sizeof(info->artist) - 1);
          memcpy(info->artist, stream, alen);
          info->artist[alen] = '\0';
          if (!info->title[0]) {
            strncpy(info->title, sep + 3, sizeof(info->title) - 1);
            info->title[sizeof(info->title) - 1] = '\0';
          }
        }
      }
    }
  }

  // Extract album art URI (relative like /getaa?s=1&u=... or absolute)
  char art_raw[256] = {};
  if (xml_extract(didl, "upnp:albumArtURI", art_raw, sizeof(art_raw)) &&
      art_raw[0]) {
    if (art_raw[0] == '/') {
      snprintf(info->art_url, sizeof(info->art_url), "http://%s:%d%s",
               s_speaker_ip, s_speaker_port, art_raw);
    } else {
      strncpy(info->art_url, art_raw, sizeof(info->art_url) - 1);
      info->art_url[sizeof(info->art_url) - 1] = '\0';
    }
  }

  detect_source(track_uri, info->source, sizeof(info->source));
  info->has_media = info->title[0] || info->artist[0];
}

// ─── Commands ───────────────────────────────────────────────────────────────

// DIDL-Lite metadata template for Spotify playlists on Sonos
static constexpr const char *SPOTIFY_PLAYLIST_META_FMT =
    "&lt;DIDL-Lite xmlns:dc=&quot;http://purl.org/dc/elements/1.1/&quot; "
    "xmlns:upnp=&quot;urn:schemas-upnp-org:metadata-1-0/upnp/&quot; "
    "xmlns:r=&quot;urn:schemas-rinconnetworks-com:metadata-1-0/&quot; "
    "xmlns=&quot;urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/&quot;&gt;"
    "&lt;item id=&quot;1006206c%s&quot; restricted=&quot;true&quot;&gt;"
    "&lt;upnp:class&gt;object.container.playlistContainer&lt;/upnp:class&gt;"
    "&lt;desc id=&quot;cdudn&quot; nameSpace=&quot;urn:schemas-rinconnetworks-com:metadata-1-0/&quot;&gt;"
    "SA_RINCON2311_X_#Svc2311-0-Token&lt;/desc&gt;"
    "&lt;/item&gt;&lt;/DIDL-Lite&gt;";

// SetAVTransportURI with metadata support
static constexpr const char *SET_URI_META_FMT =
    "<u:SetAVTransportURI xmlns:u=\"" AV_TRANSPORT_NS "\">"
    "<InstanceID>0</InstanceID>"
    "<CurrentURI>%s</CurrentURI>"
    "<CurrentURIMetaData>%s</CurrentURIMetaData>"
    "</u:SetAVTransportURI>";

static void exec_play_uri(const char *uri) {
  char fixed_uri[280];
  char metadata[768] = "";
  bool use_metadata = false;

  if (strncmp(uri, "spotify:playlist:", 17) == 0) {
    // Spotify playlist — wrap in x-rincon-cpcontainer with DIDL-Lite metadata
    snprintf(fixed_uri, sizeof(fixed_uri),
             "x-rincon-cpcontainer:1006206c%s?sid=9&flags=8300&sn=7", uri);
    snprintf(metadata, sizeof(metadata), SPOTIFY_PLAYLIST_META_FMT, uri);
    use_metadata = true;
  } else if (strncmp(uri, "spotify:album:", 14) == 0) {
    // Spotify album
    snprintf(fixed_uri, sizeof(fixed_uri),
             "x-rincon-cpcontainer:0004206c%s?sid=9&flags=8300&sn=7", uri);
    snprintf(metadata, sizeof(metadata), SPOTIFY_PLAYLIST_META_FMT, uri);
    use_metadata = true;
  } else if (strncmp(uri, "https://", 8) == 0) {
    // Internet radio (HTTPS)
    snprintf(fixed_uri, sizeof(fixed_uri), "x-rincon-mp3radio://%s", uri + 8);
  } else if (strncmp(uri, "http://", 7) == 0) {
    // Internet radio (HTTP)
    snprintf(fixed_uri, sizeof(fixed_uri), "x-rincon-mp3radio://%s", uri + 7);
  } else {
    // Pass through as-is
    strncpy(fixed_uri, uri, sizeof(fixed_uri) - 1);
    fixed_uri[sizeof(fixed_uri) - 1] = '\0';
  }

  // Build SOAP body — with or without metadata
  static char inner[1536];
  ESP_LOGI(TAG, "SetAVTransportURI: input='%s' → fixed='%s'", uri, fixed_uri);
  if (use_metadata) {
    snprintf(inner, sizeof(inner), SET_URI_META_FMT, fixed_uri, metadata);
  } else {
    snprintf(inner, sizeof(inner), SET_URI_FMT, fixed_uri);
  }

  // Use soap_request (not soap_fire) so we can log error responses
  static char resp_buf[512];
  Response resp = {resp_buf, 0, static_cast<int>(sizeof(resp_buf))};

  ESP_LOGD(TAG, "SOAP body: %.400s", inner);
  resp.len = 0;
  if (soap_request(AV_TRANSPORT_PATH, "SetAVTransportURI", AV_TRANSPORT_NS,
                   inner, &resp)) {
    resp.len = 0;
    soap_fire(AV_TRANSPORT_PATH, "Play", AV_TRANSPORT_NS, PLAY_BODY);
    ESP_LOGI(TAG, "Playing: %s → %s", uri, fixed_uri);
  }
}

static void exec_play() {
  soap_fire(AV_TRANSPORT_PATH, "Play", AV_TRANSPORT_NS, PLAY_BODY);
}

static void exec_pause() {
  soap_fire(AV_TRANSPORT_PATH, "Pause", AV_TRANSPORT_NS, PAUSE_BODY);
}

static void exec_previous() {
  soap_fire(AV_TRANSPORT_PATH, "Previous", AV_TRANSPORT_NS, PREVIOUS_BODY);
}

static void exec_next() {
  soap_fire(AV_TRANSPORT_PATH, "Next", AV_TRANSPORT_NS, NEXT_BODY);
}

static void exec_stop_playback() {
  soap_fire(AV_TRANSPORT_PATH, "Stop", AV_TRANSPORT_NS, STOP_BODY);
}

static void exec_set_volume(int level) {
  char inner[256];
  snprintf(inner, sizeof(inner), SET_VOLUME_FMT,
           std::clamp(level, VOLUME_MIN, VOLUME_MAX));
  soap_fire(RENDERING_CONTROL_PATH, "SetVolume", RENDERING_CONTROL_NS, inner);
}

// ─── Polling ────────────────────────────────────────────────────────────────

static void poll_state() {
  // Use a larger buffer for GetPositionInfo which includes DIDL metadata
  static char resp_buf[4096];
  Response resp = {resp_buf, 0, static_cast<int>(sizeof(resp_buf))};

  SonosState state = {};
  state.station_index = -1;
  state.play_state = PlayState::Unknown;

  resp.len = 0;
  if (soap_request(AV_TRANSPORT_PATH, "GetTransportInfo", AV_TRANSPORT_NS,
                   GET_TRANSPORT_INFO_BODY, &resp)) {
    char val[32];
    if (xml_extract(resp.data, "CurrentTransportState", val, sizeof(val))) {
      if (strcmp(val, "PLAYING") == 0)
        state.play_state = PlayState::Playing;
      else if (strcmp(val, "STOPPED") == 0)
        state.play_state = PlayState::Stopped;
      else if (strcmp(val, "PAUSED_PLAYBACK") == 0)
        state.play_state = PlayState::Paused;
      else if (strcmp(val, "TRANSITIONING") == 0)
        state.play_state = PlayState::Transitioning;
    }
  }

  resp.len = 0;
  if (soap_request(RENDERING_CONTROL_PATH, "GetVolume", RENDERING_CONTROL_NS,
                   GET_VOLUME_BODY, &resp)) {
    char val[8];
    if (xml_extract(resp.data, "CurrentVolume", val, sizeof(val))) {
      state.volume = std::clamp(atoi(val), VOLUME_MIN, VOLUME_MAX);
    }
  }

  // Get current track info (URI + metadata) for now-playing display
  resp.len = 0;
  if (soap_request(AV_TRANSPORT_PATH, "GetPositionInfo", AV_TRANSPORT_NS,
                   GET_POSITION_INFO_BODY, &resp)) {
    static char track_uri[300];
    track_uri[0] = '\0';
    xml_extract(resp.data, "TrackURI", track_uri, sizeof(track_uri));

    ESP_LOGD(TAG, "TrackURI: %.200s", track_uri);

    // Check if the URI matches one of the registered station URLs.
    // Use strstr on the host+path portion — Sonos may return the URI with
    // a different scheme prefix than what we sent.
    for (int i = 0; i < s_station_count; i++) {
      const char *host = s_station_urls[i];
      if (strncmp(host, "https://", 8) == 0)
        host += 8;
      else if (strncmp(host, "http://", 7) == 0)
        host += 7;

      if (host[0] && strstr(track_uri, host)) {
        state.station_index = i;
        break;
      }
    }

    // Only parse metadata for external media (not our stations)
    if (state.station_index < 0) {
      static char metadata[3072];
      metadata[0] = '\0';
      if (xml_extract(resp.data, "TrackMetaData", metadata, sizeof(metadata)))
        parse_media_info(metadata, track_uri, &state.media);
    }
  }

  esp_event_post(APP_EVENT, APP_EVENT_SONOS_STATE_UPDATE, &state, sizeof(state),
                 0);
}

// ─── Task ───────────────────────────────────────────────────────────────────

static void net_task(void *) {
  TickType_t poll_ticks = pdMS_TO_TICKS(CONFIG_RADIO_SONOS_POLL_INTERVAL_MS);
  TickType_t last_poll = 0;

  while (s_running) {
    Command cmd;
    if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(200)) == pdTRUE) {
      switch (cmd.type) {
      case CmdType::PlayUri:
        exec_play_uri(cmd.uri);
        break;
      case CmdType::Play:
        exec_play();
        break;
      case CmdType::Pause:
        exec_pause();
        break;
      case CmdType::Stop:
        exec_stop_playback();
        break;
      case CmdType::SetVolume:
        exec_set_volume(cmd.volume);
        break;
      case CmdType::Previous:
        exec_previous();
        break;
      case CmdType::Next:
        exec_next();
        break;
      }
    }

    TickType_t now = xTaskGetTickCount();
    if (s_running && (now - last_poll) >= poll_ticks) {
      poll_state();
      last_poll = now;
    }
  }

  vTaskDelete(nullptr);
}

// ─── Public API ─────────────────────────────────────────────────────────────

static void enqueue(const Command &cmd) {
  if (s_cmd_queue) {
    xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(50));
  }
}

void sonos_init() {
  settings_get_speaker_ip(s_speaker_ip, sizeof(s_speaker_ip));
  s_cmd_queue = xQueueCreate(8, sizeof(Command));
  ESP_LOGI(TAG, "Initialized — speaker: %s:%d", s_speaker_ip, s_speaker_port);
}

void sonos_set_speaker(const char *ip, int port) {
  strncpy(s_speaker_ip, ip, sizeof(s_speaker_ip) - 1);
  s_speaker_ip[sizeof(s_speaker_ip) - 1] = '\0';
  s_speaker_port = port;
  settings_set_speaker_ip(ip);
  ESP_LOGI(TAG, "Speaker changed: %s:%d", s_speaker_ip, s_speaker_port);
}

void sonos_start() {
  if (s_task)
    return;
  s_running = true;
  xTaskCreatePinnedToCore(net_task, "net", NET_TASK_STACK, nullptr,
                          NET_TASK_PRIO, &s_task, NET_TASK_CORE);
}

void sonos_stop() {
  s_running = false;
  if (s_task) {
    xQueueReset(s_cmd_queue);
    s_task = nullptr;
  }
}

void sonos_play_uri(const char *uri) {
  Command cmd = {.type = CmdType::PlayUri, .uri = {}};
  strncpy(cmd.uri, uri, sizeof(cmd.uri) - 1);
  enqueue(cmd);
}

void sonos_play() {
  Command cmd = {.type = CmdType::Play, .uri = {}};
  enqueue(cmd);
}

void sonos_pause() {
  Command cmd = {.type = CmdType::Pause, .uri = {}};
  enqueue(cmd);
}

void sonos_stop_playback() {
  Command cmd = {.type = CmdType::Stop, .uri = {}};
  enqueue(cmd);
}

void sonos_set_volume(int level) {
  Command cmd = {.type = CmdType::SetVolume, .volume = {}};
  cmd.volume = std::clamp(level, VOLUME_MIN, VOLUME_MAX);
  enqueue(cmd);
}

void sonos_previous() {
  Command cmd = {.type = CmdType::Previous, .uri = {}};
  enqueue(cmd);
}

void sonos_next() {
  Command cmd = {.type = CmdType::Next, .uri = {}};
  enqueue(cmd);
}

int sonos_fetch_art(const char *url, uint8_t *buf, int buf_size) {
  if (!url || !url[0] || !buf || buf_size <= 0)
    return 0;

  ESP_LOGI(TAG, "Fetching art: %.120s", url);

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.method = HTTP_METHOD_GET;
  cfg.timeout_ms = 8000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 1024;

  auto *client = esp_http_client_init(&cfg);
  if (!client)
    return 0;

  // Use open/read instead of perform to avoid event loop spam
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Art open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return 0;
  }

  int content_len = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);

  if (status != 200) {
    ESP_LOGW(TAG, "Art HTTP %d (content_len=%d)", status, content_len);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return 0;
  }

  // Read response body into buffer
  int total = 0;
  while (total < buf_size) {
    int read_len = esp_http_client_read(
        client, reinterpret_cast<char *>(buf + total), buf_size - total);
    if (read_len <= 0)
      break;
    total += read_len;
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (total <= 0) {
    ESP_LOGW(TAG, "Art: no data received");
    return 0;
  }

  ESP_LOGI(TAG, "Album art downloaded: %d bytes (JPEG: %s)", total,
           (total > 2 && buf[0] == 0xFF && buf[1] == 0xD8) ? "yes" : "no");
  return total;
}
