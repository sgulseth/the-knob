#include "ui.h"
#include "app_config.h"
#include "art_decoder.h"
#include "fonts.h"
#include "encoder.h"
#include "haptic.h"
#include "sonos.h"
#include "settings.h"
#include "display.h"
#include "ui_pages.h"
#include "ui/ui_timer.h"
#include "ui/ui_voice.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

static constexpr const char *TAG = "ui";

// ─── Event IDs (must match main.cpp) ────────────────────────────────────────

enum : int32_t {
  APP_EVENT_PLAYLIST_PLAY_REQUESTED = 210,
};

// ─── Timing Constants ───────────────────────────────────────────────────────

static constexpr int BROWSE_TIMEOUT_MS = 7000;
static constexpr int VOL_DISPLAY_MS = 1500;
static constexpr int VOL_LOCAL_GRACE_MS = 2000;
static constexpr int ENCODER_POLL_MS = 20;
static constexpr int ANIM_FADE_MS = 200;
static constexpr int ANIM_QUICK_MS = 100;
static constexpr int ANIM_ARC_FADE_MS = 400;
static constexpr int ANIM_BG_FADE_MS = 250;
static constexpr int LONG_PRESS_MS = 500;
static constexpr int ART_BUF_SIZE = 128 * 1024;

static constexpr uint8_t BACKLIGHT_NORMAL = 80;
static constexpr uint8_t BACKLIGHT_DIM = 8;
static constexpr int BACKLIGHT_FADE_STEP_MS = 30;
static constexpr int BACKLIGHT_INACTIVITY_MS = 15000;

// ─── Palette ────────────────────────────────────────────────────────────────

#define COL_BG lv_color_hex(0x000000)
#define COL_TEXT lv_color_hex(0xFFFFFF)
#define COL_TEXT_SEC lv_color_hex(0x8E8E93)
#define COL_ACCENT lv_color_hex(0x0A84FF)
#define COL_ARC_BG lv_color_hex(0x1C1C1E)
#define COL_ARC_ACTIVE lv_color_hex(0xFFFFFF)
#define COL_ARC_DIM lv_color_hex(0x555555)
#define COL_GREEN lv_color_hex(0x30D158)
#define COL_BROWSE_BG lv_color_hex(0x0A0A0A)

// ─── State ──────────────────────────────────────────────────────────────────

static AppScreen s_screen_state = AppScreen::ModeSelect;
static JukeboxMode s_selected_mode = JukeboxMode::Radio;
static int s_volume;
static int s_radio_index = 0;
static int s_user_index = 0;
static int s_playlist_index = 0;
static PlayState s_play_state = PlayState::Stopped;
static bool s_idle_active = false;
static MediaInfo s_media = {};
static bool s_external_playing = false;
static bool s_voice_active = false;

// ─── Widgets ────────────────────────────────────────────────────────────────

static lv_obj_t *s_screen;
static lv_obj_t *s_home;

// Status bar
static lv_obj_t *s_wifi_dot;

// Background layers (two-layer crossfade)
static lv_obj_t *s_bg_back;
static lv_obj_t *s_bg_front;
static lv_obj_t *s_bg_dim;

// Artwork container
static lv_obj_t *s_logo_container;
static lv_obj_t *s_img_logo;

// Labels
static lv_obj_t *s_lbl_title;
static lv_obj_t *s_lbl_subtitle;
static lv_obj_t *s_lbl_position;
static lv_obj_t *s_lbl_clock;
static lv_obj_t *s_lbl_speaker;

// Prev/Next buttons (playlist now-playing)
static lv_obj_t *s_btn_prev;
static lv_obj_t *s_btn_next;

// Volume arc
static lv_obj_t *s_vol_arc;
static lv_timer_t *s_vol_hide_timer;
static int32_t s_arc_display_val;
static uint32_t s_local_vol_ms;

// Browse timeout
static lv_timer_t *s_browse_timer;

// Touch / press detection
static lv_timer_t *s_press_timer;
static bool s_press_was_long;

// Clock
static lv_timer_t *s_clock_timer;

// Backlight
static lv_timer_t *s_bl_timer;
static lv_timer_t *s_bl_inactivity_timer;
static uint8_t s_bl_current = BACKLIGHT_NORMAL;
static uint8_t s_bl_target = BACKLIGHT_NORMAL;

// Album art
static uint8_t *s_art_jpeg;
static uint8_t *s_art_pixels;
static lv_image_dsc_t s_art_dsc;
static char s_art_last_url[256];

// Speaker picker
static lv_obj_t *s_scr_speaker_picker;
static lv_obj_t *s_scanning_overlay;
static lv_obj_t *s_lbl_scanning;
static DiscoveryResult s_discovered;
static int s_speaker_highlight;
static bool s_on_picker;

// ─── Forward Declarations ───────────────────────────────────────────────────

static void transition_to(AppScreen screen);
static void go_back();
static void do_tap();
static void do_long_press();
static void update_screen_content();
static void update_clock();
static bool should_idle();
static void show_idle_ui(bool idle);
static void on_page_changed(int index, const char *id);
static void on_encoder_poll(lv_timer_t *);
static void on_prev_tap(lv_event_t *);
static void on_next_tap(lv_event_t *);

// ─── Animation Helpers ──────────────────────────────────────────────────────

static void anim_opa_cb(void *obj, int32_t v) {
  lv_obj_set_style_opa(static_cast<lv_obj_t *>(obj), v, LV_PART_MAIN);
}

static void anim_arc_ind_opa_cb(void *obj, int32_t v) {
  lv_obj_set_style_arc_opa(static_cast<lv_obj_t *>(obj), v, LV_PART_INDICATOR);
}

static void anim_vol_arc_cb(void *obj, int32_t v) {
  s_arc_display_val = v;
  lv_arc_set_value(static_cast<lv_obj_t *>(obj), v);
}

static void anim_bg_opa_cb(void *obj, int32_t v) {
  lv_obj_set_style_bg_opa(static_cast<lv_obj_t *>(obj), v, LV_PART_MAIN);
}

static void anim_hide_done(lv_anim_t *a) {
  lv_obj_add_flag(static_cast<lv_obj_t *>(a->var), LV_OBJ_FLAG_HIDDEN);
}

static void anim_fade(lv_obj_t *obj, lv_anim_exec_xcb_t exec_cb, int32_t start,
                      int32_t end, int duration,
                      void (*done_cb)(lv_anim_t *) = nullptr) {
  if (duration <= 0) {
    exec_cb(obj, end);
    if (done_cb) {
      lv_anim_t dummy = {};
      lv_anim_set_var(&dummy, obj);
      done_cb(&dummy);
    }
    return;
  }

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, exec_cb);
  lv_anim_set_values(&a, start, end);
  lv_anim_set_duration(&a, duration);
  lv_anim_set_path_cb(&a, duration <= 150 ? lv_anim_path_linear
                                          : lv_anim_path_ease_in_out);
  if (done_cb)
    lv_anim_set_completed_cb(&a, done_cb);
  lv_anim_start(&a);
}

// ─── Background Crossfade ───────────────────────────────────────────────────

static void anim_bg_crossfade_cb(void *obj, int32_t v) {
  lv_obj_set_style_bg_opa(static_cast<lv_obj_t *>(obj), v, LV_PART_MAIN);
}

static void anim_bg_crossfade_done(lv_anim_t *) {
  lv_color_t c = lv_obj_get_style_bg_color(s_bg_front, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_bg_back, c, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_bg_front, LV_OPA_TRANSP, LV_PART_MAIN);
  if (s_logo_container)
    lv_obj_set_style_bg_color(s_logo_container, c, LV_PART_MAIN);
}

static void set_bg_color(uint32_t hex, bool animate) {
  lv_color_t target = lv_color_hex(hex);

  lv_anim_delete(s_bg_front, anim_bg_crossfade_cb);
  int32_t front_opa = lv_obj_get_style_bg_opa(s_bg_front, LV_PART_MAIN);
  if (front_opa > LV_OPA_TRANSP) {
    lv_color_t fc = lv_obj_get_style_bg_color(s_bg_front, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bg_back, fc, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bg_front, LV_OPA_TRANSP, LV_PART_MAIN);
  }

  lv_color_t current = lv_obj_get_style_bg_color(s_bg_back, LV_PART_MAIN);
  if (current.red == target.red && current.green == target.green &&
      current.blue == target.blue)
    return;

  if (!animate) {
    lv_obj_set_style_bg_color(s_bg_back, target, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bg_front, LV_OPA_TRANSP, LV_PART_MAIN);
    if (s_logo_container)
      lv_obj_set_style_bg_color(s_logo_container, target, LV_PART_MAIN);
    return;
  }

  lv_obj_set_style_bg_color(s_bg_front, target, LV_PART_MAIN);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, s_bg_front);
  lv_anim_set_exec_cb(&a, anim_bg_crossfade_cb);
  lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_duration(&a, ANIM_BG_FADE_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_completed_cb(&a, anim_bg_crossfade_done);
  lv_anim_start(&a);
}

// ─── Backlight ──────────────────────────────────────────────────────────────

static void on_backlight_step(lv_timer_t *) {
  if (s_bl_current == s_bl_target) {
    lv_timer_pause(s_bl_timer);
    return;
  }
  if (s_bl_current < s_bl_target)
    s_bl_current = std::min<uint8_t>(s_bl_current + 3, s_bl_target);
  else
    s_bl_current = (s_bl_current > s_bl_target + 3)
                       ? static_cast<uint8_t>(s_bl_current - 3)
                       : s_bl_target;
  display_set_backlight(s_bl_current);
}

static void backlight_fade_to(uint8_t target) {
  s_bl_target = target;
  if (s_bl_current == target)
    return;
  lv_timer_reset(s_bl_timer);
  lv_timer_resume(s_bl_timer);
}

static void on_bl_inactivity(lv_timer_t *) {
  lv_timer_pause(s_bl_inactivity_timer);
  if (!s_idle_active)
    backlight_fade_to(BACKLIGHT_DIM);
}

static void backlight_poke() {
  backlight_fade_to(BACKLIGHT_NORMAL);
  if (s_bl_inactivity_timer) {
    lv_timer_reset(s_bl_inactivity_timer);
    lv_timer_resume(s_bl_inactivity_timer);
  }
}

// ─── Volume Arc ─────────────────────────────────────────────────────────────

static void on_vol_hide(lv_timer_t *) {
  anim_fade(s_vol_arc, anim_arc_ind_opa_cb, LV_OPA_COVER, LV_OPA_30,
            ANIM_ARC_FADE_MS);
  lv_timer_pause(s_vol_hide_timer);
  if (s_idle_active)
    backlight_fade_to(BACKLIGHT_DIM);
}

static void show_volume(int level) {
  backlight_poke();
  lv_anim_delete(s_vol_arc, anim_arc_ind_opa_cb);
  lv_obj_set_style_arc_opa(s_vol_arc, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_vol_arc, COL_ARC_ACTIVE, LV_PART_INDICATOR);

  lv_anim_delete(s_vol_arc, anim_vol_arc_cb);
  s_arc_display_val = level;
  lv_arc_set_value(s_vol_arc, level);

  lv_timer_reset(s_vol_hide_timer);
  lv_timer_resume(s_vol_hide_timer);
}

// ─── Clock ──────────────────────────────────────────────────────────────────

static void update_clock() {
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
  lv_label_set_text(s_lbl_clock, buf);
}

static void on_clock_tick(lv_timer_t *) { update_clock(); }

// ─── Idle State ─────────────────────────────────────────────────────────────

static bool should_idle() {
  return s_play_state == PlayState::Stopped &&
         s_screen_state == AppScreen::ModeSelect;
}

static void show_idle_ui(bool idle) {
  if (!pages_is_home())
    return;
  if (idle == s_idle_active)
    return;
  s_idle_active = idle;

  if (idle) {
    backlight_fade_to(BACKLIGHT_DIM);
    lv_timer_pause(s_bl_inactivity_timer);
  } else {
    backlight_poke();
  }

  lv_anim_delete(s_lbl_clock, anim_opa_cb);
  lv_anim_delete(s_logo_container, anim_opa_cb);
  lv_anim_delete(s_lbl_title, anim_opa_cb);
  lv_anim_delete(s_lbl_speaker, anim_opa_cb);
  lv_anim_delete(s_lbl_subtitle, anim_opa_cb);
  lv_anim_delete(s_bg_dim, anim_bg_opa_cb);

  if (idle) {
    update_clock();
    lv_obj_remove_flag(s_lbl_clock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_lbl_clock, LV_OPA_TRANSP, LV_PART_MAIN);
    anim_fade(s_lbl_clock, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_90,
              ANIM_FADE_MS);

    anim_fade(s_logo_container, anim_opa_cb,
              lv_obj_get_style_opa(s_logo_container, LV_PART_MAIN),
              LV_OPA_TRANSP, ANIM_FADE_MS, anim_hide_done);
    anim_fade(s_lbl_title, anim_opa_cb, LV_OPA_COVER, LV_OPA_TRANSP,
              ANIM_FADE_MS, anim_hide_done);
    anim_fade(s_lbl_speaker, anim_opa_cb, LV_OPA_60, LV_OPA_TRANSP,
              ANIM_FADE_MS, anim_hide_done);
    anim_fade(s_bg_dim, anim_bg_opa_cb, LV_OPA_TRANSP, LV_OPA_70,
              ANIM_FADE_MS);

    lv_obj_set_style_opa(s_lbl_subtitle, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_align(s_lbl_subtitle, LV_ALIGN_CENTER, 0, 30);
    anim_fade(s_lbl_subtitle, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER,
              ANIM_FADE_MS);

    lv_timer_resume(s_clock_timer);
  } else {
    anim_fade(s_lbl_clock, anim_opa_cb,
              lv_obj_get_style_opa(s_lbl_clock, LV_PART_MAIN), LV_OPA_TRANSP,
              ANIM_QUICK_MS, anim_hide_done);

    lv_obj_remove_flag(s_lbl_title, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_lbl_speaker, LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_style_opa(s_lbl_title, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_opa(s_lbl_speaker, LV_OPA_TRANSP, LV_PART_MAIN);

    anim_fade(s_lbl_title, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER,
              ANIM_FADE_MS);
    anim_fade(s_lbl_speaker, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_60,
              ANIM_FADE_MS);

    anim_fade(s_bg_dim, anim_bg_opa_cb,
              lv_obj_get_style_bg_opa(s_bg_dim, LV_PART_MAIN), LV_OPA_TRANSP,
              ANIM_FADE_MS);

    lv_obj_set_style_opa(s_lbl_subtitle, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_align(s_lbl_subtitle, LV_ALIGN_CENTER, 0, 94);
    anim_fade(s_lbl_subtitle, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER,
              ANIM_FADE_MS);

    lv_timer_pause(s_clock_timer);
  }
}

// ─── Screen Content Updates ─────────────────────────────────────────────────

static void update_screen_content() {
  // Hide position by default
  lv_obj_add_flag(s_lbl_position, LV_OBJ_FLAG_HIDDEN);
  // Hide prev/next by default
  lv_obj_add_flag(s_btn_prev, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_btn_next, LV_OBJ_FLAG_HIDDEN);
  // Hide logo container by default
  lv_obj_add_flag(s_logo_container, LV_OBJ_FLAG_HIDDEN);

  // Reset subtitle position
  lv_obj_align(s_lbl_subtitle, LV_ALIGN_CENTER, 0, 94);
  lv_label_set_long_mode(s_lbl_subtitle, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_lbl_subtitle, LV_SIZE_CONTENT);
  lv_obj_set_style_text_color(s_lbl_subtitle, lv_color_hex(0xAAAAAA),
                              LV_PART_MAIN);

  // Reset title style
  lv_obj_set_style_text_color(s_lbl_title, COL_TEXT, LV_PART_MAIN);
  lv_obj_align(s_lbl_title, LV_ALIGN_CENTER, 0, 0);

  char pos_buf[24];

  switch (s_screen_state) {
  case AppScreen::ModeSelect: {
    const char *mode_name = (s_selected_mode == JukeboxMode::Radio)
                                ? "Radio"
                                : "Playlists";
    lv_label_set_text(s_lbl_title, mode_name);
    lv_label_set_text(s_lbl_subtitle, "Tap to select");
    set_bg_color(0x0A0A0A, true);
    break;
  }

  case AppScreen::RadioBrowse: {
    lv_label_set_text(s_lbl_title, RADIO_STATIONS[s_radio_index].name);
    lv_label_set_text(s_lbl_subtitle, "Tap to play");
    snprintf(pos_buf, sizeof(pos_buf), "%d / %d", s_radio_index + 1,
             RADIO_STATION_COUNT);
    lv_label_set_text(s_lbl_position, pos_buf);
    lv_obj_remove_flag(s_lbl_position, LV_OBJ_FLAG_HIDDEN);
    set_bg_color(RADIO_STATIONS[s_radio_index].color, true);
    break;
  }

  case AppScreen::UserSelect: {
    lv_label_set_text(s_lbl_title, USERS[s_user_index].name);
    lv_label_set_text(s_lbl_subtitle, "Tap for playlists");
    snprintf(pos_buf, sizeof(pos_buf), "%d / %d", s_user_index + 1,
             USER_COUNT);
    lv_label_set_text(s_lbl_position, pos_buf);
    lv_obj_remove_flag(s_lbl_position, LV_OBJ_FLAG_HIDDEN);
    set_bg_color(USERS[s_user_index].color, true);
    break;
  }

  case AppScreen::PlaylistSelect: {
    auto &user = USERS[s_user_index];
    lv_label_set_text(s_lbl_title, user.playlists[s_playlist_index].name);
    lv_label_set_text(s_lbl_subtitle, user.name);
    snprintf(pos_buf, sizeof(pos_buf), "%d / %d", s_playlist_index + 1,
             user.playlist_count);
    lv_label_set_text(s_lbl_position, pos_buf);
    lv_obj_remove_flag(s_lbl_position, LV_OBJ_FLAG_HIDDEN);
    set_bg_color(user.color, true);
    break;
  }

  case AppScreen::NowPlaying: {
    // Show album art container
    lv_obj_remove_flag(s_logo_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_logo_container, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(s_lbl_title, LV_ALIGN_CENTER, 0, 68);

    if (s_external_playing && s_media.has_media) {
      lv_label_set_text(s_lbl_title,
                        s_media.title[0] ? s_media.title : "Unknown");
      if (s_media.artist[0]) {
        lv_obj_set_width(s_lbl_subtitle, LCD_H_RES - 100);
        lv_label_set_long_mode(s_lbl_subtitle, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(s_lbl_subtitle, LV_TEXT_ALIGN_CENTER,
                                    LV_PART_MAIN);
        lv_label_set_text(s_lbl_subtitle, s_media.artist);
      } else {
        lv_label_set_text(s_lbl_subtitle,
                          s_play_state == PlayState::Paused ? "Paused"
                                                           : "Playing");
      }
    } else if (s_selected_mode == JukeboxMode::Radio) {
      lv_label_set_text(s_lbl_title, RADIO_STATIONS[s_radio_index].name);
      lv_label_set_text(s_lbl_subtitle,
                        s_play_state == PlayState::Paused ? "Paused"
                                                         : "Playing");
    } else {
      auto &user = USERS[s_user_index];
      lv_label_set_text(s_lbl_title,
                        user.playlists[s_playlist_index].name);
      lv_label_set_text(s_lbl_subtitle,
                        s_play_state == PlayState::Paused ? "Paused"
                                                         : user.name);
    }

    // Show prev/next for playlist mode
    if (s_selected_mode == JukeboxMode::Playlist) {
      lv_obj_remove_flag(s_btn_prev, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(s_btn_next, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_selected_mode == JukeboxMode::Radio)
      set_bg_color(RADIO_STATIONS[s_radio_index].color, true);
    else
      set_bg_color(USERS[s_user_index].color, true);
    break;
  }
  }
}

// ─── Screen Transitions ─────────────────────────────────────────────────────

static void transition_to(AppScreen screen) {
  AppScreen prev = s_screen_state;
  s_screen_state = screen;

  // Cancel browse timer
  lv_timer_pause(s_browse_timer);

  // Exit idle if needed
  if (s_idle_active)
    show_idle_ui(false);

  // Reset playlist index when entering PlaylistSelect
  if (screen == AppScreen::PlaylistSelect && prev != AppScreen::PlaylistSelect)
    s_playlist_index = 0;

  // Volume cap on source change (entering NowPlaying = starting playback)
  if (screen == AppScreen::NowPlaying && prev != AppScreen::NowPlaying) {
    if (s_volume > VOLUME_CAP_ON_SOURCE_CHANGE) {
      s_volume = VOLUME_CAP_ON_SOURCE_CHANGE;
      sonos_set_volume(s_volume);
      settings_set_volume(s_volume);
      lv_arc_set_value(s_vol_arc, s_volume);
      s_arc_display_val = s_volume;
    }
  }

  // Start browse timer for browse screens
  if (screen == AppScreen::RadioBrowse ||
      screen == AppScreen::UserSelect ||
      screen == AppScreen::PlaylistSelect) {
    lv_timer_reset(s_browse_timer);
    lv_timer_resume(s_browse_timer);
  }

  // Fade out title, update, fade in
  lv_anim_delete(s_lbl_title, anim_opa_cb);
  lv_anim_delete(s_lbl_subtitle, anim_opa_cb);
  lv_obj_set_style_opa(s_lbl_title, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_opa(s_lbl_subtitle, LV_OPA_TRANSP, LV_PART_MAIN);

  update_screen_content();

  anim_fade(s_lbl_title, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER,
            ANIM_FADE_MS);
  anim_fade(s_lbl_subtitle, anim_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER,
            ANIM_FADE_MS);

  // Check idle
  if (should_idle())
    show_idle_ui(true);
}

// ─── Navigation ─────────────────────────────────────────────────────────────

static void go_back() {
  switch (s_screen_state) {
  case AppScreen::ModeSelect:
    // Already at top — no-op
    break;
  case AppScreen::RadioBrowse:
    transition_to(AppScreen::ModeSelect);
    break;
  case AppScreen::UserSelect:
    transition_to(AppScreen::ModeSelect);
    break;
  case AppScreen::PlaylistSelect:
    transition_to(AppScreen::UserSelect);
    break;
  case AppScreen::NowPlaying:
    transition_to(AppScreen::ModeSelect);
    break;
  }
}

static void on_browse_timeout(lv_timer_t *) {
  lv_timer_pause(s_browse_timer);
  if (s_play_state == PlayState::Playing ||
      s_play_state == PlayState::Transitioning) {
    transition_to(AppScreen::NowPlaying);
  } else {
    go_back();
  }
}

// ─── Tap Handling ───────────────────────────────────────────────────────────

static void do_tap() {
  if (s_on_picker)
    return;

  switch (s_screen_state) {
  case AppScreen::ModeSelect:
    if (s_idle_active)
      show_idle_ui(false);
    if (s_selected_mode == JukeboxMode::Radio)
      transition_to(AppScreen::RadioBrowse);
    else
      transition_to(AppScreen::UserSelect);
    break;

  case AppScreen::RadioBrowse: {
    // Play selected radio station
    int32_t idx = s_radio_index;
    esp_event_post(APP_EVENT, APP_EVENT_STATION_CHANGED, &idx, sizeof(idx), 0);
    esp_event_post(APP_EVENT, APP_EVENT_PLAY_REQUESTED, nullptr, 0, 0);
    s_play_state = PlayState::Playing;
    s_external_playing = false;
    s_media = {};
    transition_to(AppScreen::NowPlaying);
    break;
  }

  case AppScreen::UserSelect:
    s_playlist_index = 0;
    transition_to(AppScreen::PlaylistSelect);
    break;

  case AppScreen::PlaylistSelect: {
    // Play selected playlist
    auto &user = USERS[s_user_index];
    const char *uri = user.playlists[s_playlist_index].uri;
    esp_event_post(APP_EVENT, APP_EVENT_PLAYLIST_PLAY_REQUESTED,
                   uri, strlen(uri) + 1, 0);
    s_play_state = PlayState::Playing;
    s_external_playing = false;
    s_media = {};
    transition_to(AppScreen::NowPlaying);
    break;
  }

  case AppScreen::NowPlaying:
    // Toggle pause/play
    if (s_play_state == PlayState::Playing) {
      sonos_pause();
      s_play_state = PlayState::Paused;
    } else if (s_play_state == PlayState::Paused) {
      sonos_play();
      s_play_state = PlayState::Playing;
    }
    update_screen_content();
    break;
  }
}

// ─── Voice Mode ─────────────────────────────────────────────────────────────

static int s_pre_voice_volume;
static AppScreen s_pre_voice_screen;

static void activate_voice() {
  if (s_voice_active)
    return;
  s_voice_active = true;
  s_pre_voice_volume = s_volume;
  s_pre_voice_screen = s_screen_state;
  voice_ui_enter();
  sonos_set_volume(VOICE_DUCKED_VOLUME);
  esp_event_post(APP_EVENT, APP_EVENT_VOICE_ACTIVATE, nullptr, 0, 0);
}

static void deactivate_voice() {
  if (!s_voice_active)
    return;
  s_voice_active = false;
  voice_ui_exit();
  sonos_set_volume(s_pre_voice_volume);
  esp_event_post(APP_EVENT, APP_EVENT_VOICE_DEACTIVATE, nullptr, 0, 0);
  update_screen_content();
}

static void do_long_press() {
  if (s_on_picker)
    return;

  if (ui_is_voice_active()) {
    deactivate_voice();
  } else {
    activate_voice();
  }
}

// ─── Touch Events ───────────────────────────────────────────────────────────

static void on_press_timer(lv_timer_t *) {
  s_press_was_long = true;
  lv_timer_pause(s_press_timer);
  do_long_press();
}

static void on_screen_pressed(lv_event_t *) {
  s_press_was_long = false;
  lv_timer_reset(s_press_timer);
  lv_timer_resume(s_press_timer);
}

static void on_screen_released(lv_event_t *) {
  lv_timer_pause(s_press_timer);
  if (s_press_was_long)
    return;

  backlight_poke();
  pages_poke();

  if (ui_is_voice_active()) {
    deactivate_voice();
    return;
  }

  if (!pages_is_home()) {
    pages_go_home();
    return;
  }

  do_tap();
}

static void on_screen_gesture(lv_event_t *e) {
  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
  if (dir == LV_DIR_TOP) {
    backlight_poke();
    pages_poke();
    if (ui_is_voice_active()) {
      deactivate_voice();
    } else {
      go_back();
    }
  }
}

// ─── Prev/Next Buttons ─────────────────────────────────────────────────────

static void on_prev_tap(lv_event_t *) {
  backlight_poke();
  haptic_buzz();
  sonos_previous();
}

static void on_next_tap(lv_event_t *) {
  backlight_poke();
  haptic_buzz();
  sonos_next();
}

// ─── Encoder ────────────────────────────────────────────────────────────────

static void handle_encoder(int32_t steps) {
  backlight_poke();

  if (s_on_picker) {
    if (s_discovered.count > 0) {
      s_speaker_highlight =
          std::clamp(s_speaker_highlight + static_cast<int>(steps), 0,
                     s_discovered.count - 1);
      highlight_picker_item(s_speaker_highlight);
    }
    return;
  }

  if (ui_is_voice_active())
    return;

  if (!pages_is_home()) {
    pages_navigate(steps > 0 ? 1 : -1);
    pages_poke();
    return;
  }

  pages_poke();

  switch (s_screen_state) {
  case AppScreen::ModeSelect: {
    if (s_idle_active) {
      // When idle, encoder = volume
      int raw = s_volume + static_cast<int>(steps) * VOLUME_STEP;
      s_volume = std::clamp(raw, VOLUME_MIN, VOLUME_MAX);
      if (raw < VOLUME_MIN || raw > VOLUME_MAX)
        haptic_buzz();
      show_volume(s_volume);
      s_local_vol_ms = lv_tick_get();
      int32_t vol = s_volume;
      esp_event_post(APP_EVENT, APP_EVENT_VOLUME_CHANGED, &vol, sizeof(vol), 0);
      sonos_set_volume(s_volume);
    } else {
      // Cycle modes
      s_selected_mode = (s_selected_mode == JukeboxMode::Radio)
                            ? JukeboxMode::Playlist
                            : JukeboxMode::Radio;
      haptic_buzz();
      update_screen_content();
    }
    break;
  }

  case AppScreen::RadioBrowse: {
    int idx = s_radio_index + static_cast<int>(steps);
    s_radio_index = ((idx % RADIO_STATION_COUNT) + RADIO_STATION_COUNT) %
                    RADIO_STATION_COUNT;
    haptic_buzz();
    update_screen_content();
    lv_timer_reset(s_browse_timer);
    break;
  }

  case AppScreen::UserSelect: {
    int idx = s_user_index + static_cast<int>(steps);
    s_user_index = ((idx % USER_COUNT) + USER_COUNT) % USER_COUNT;
    haptic_buzz();
    update_screen_content();
    lv_timer_reset(s_browse_timer);
    break;
  }

  case AppScreen::PlaylistSelect: {
    int count = USERS[s_user_index].playlist_count;
    int idx = s_playlist_index + static_cast<int>(steps);
    s_playlist_index = ((idx % count) + count) % count;
    haptic_buzz();
    update_screen_content();
    lv_timer_reset(s_browse_timer);
    break;
  }

  case AppScreen::NowPlaying: {
    // Volume control
    int raw = s_volume + static_cast<int>(steps) * VOLUME_STEP;
    s_volume = std::clamp(raw, VOLUME_MIN, VOLUME_MAX);
    if (raw < VOLUME_MIN || raw > VOLUME_MAX)
      haptic_buzz();
    show_volume(s_volume);
    s_local_vol_ms = lv_tick_get();
    int32_t vol = s_volume;
    esp_event_post(APP_EVENT, APP_EVENT_VOLUME_CHANGED, &vol, sizeof(vol), 0);
    sonos_set_volume(s_volume);
    break;
  }
  }
}

static void on_encoder_poll(lv_timer_t *) {
  int32_t steps = encoder_take_steps();
  if (steps != 0)
    handle_encoder(steps);
}

// ─── Home Page (page 0 in pager) ────────────────────────────────────────────

static void home_page_build(lv_obj_t *parent) {
  s_home = parent;

  // ── Background layers ──
  auto make_bg_layer = [&](lv_color_t color, lv_opa_t opa) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, LCD_H_RES, LCD_V_RES);
    lv_obj_align(obj, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(obj, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, opa, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
  };
  s_bg_back = make_bg_layer(lv_color_hex(0x0A0A0A), LV_OPA_COVER);
  s_bg_front = make_bg_layer(lv_color_black(), LV_OPA_TRANSP);

  // ── Dim overlay ──
  s_bg_dim = lv_obj_create(parent);
  lv_obj_set_size(s_bg_dim, LCD_H_RES, LCD_V_RES);
  lv_obj_align(s_bg_dim, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(s_bg_dim, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_bg_dim, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_bg_dim, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(s_bg_dim, 0, LV_PART_MAIN);
  lv_obj_remove_flag(s_bg_dim, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(s_bg_dim, LV_OBJ_FLAG_SCROLLABLE);

  // ── Volume arc ──
  s_vol_arc = lv_arc_create(parent);
  lv_obj_set_size(s_vol_arc, LCD_H_RES - 4, LCD_V_RES - 4);
  lv_obj_center(s_vol_arc);
  lv_arc_set_rotation(s_vol_arc, 135);
  lv_arc_set_bg_angles(s_vol_arc, 0, 270);
  lv_arc_set_range(s_vol_arc, VOLUME_MIN, VOLUME_MAX);
  lv_arc_set_value(s_vol_arc, s_volume);
  s_arc_display_val = s_volume;
  lv_obj_remove_flag(s_vol_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(s_vol_arc, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_color(s_vol_arc, COL_ARC_BG, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(s_vol_arc, true, LV_PART_MAIN);
  lv_obj_set_style_arc_width(s_vol_arc, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_vol_arc, COL_ARC_ACTIVE, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(s_vol_arc, true, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s_vol_arc, LV_OPA_TRANSP, LV_PART_KNOB);

  // ── Logo / art container ──
  s_logo_container = lv_obj_create(parent);
  lv_obj_set_size(s_logo_container, 120, 120);
  lv_obj_set_style_bg_color(s_logo_container, lv_color_hex(0x0A0A0A),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_logo_container, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_logo_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_logo_container, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(s_logo_container, 28, LV_PART_MAIN);
  lv_obj_set_style_clip_corner(s_logo_container, true, LV_PART_MAIN);
  lv_obj_remove_flag(s_logo_container, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(s_logo_container, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(s_logo_container, LV_ALIGN_CENTER, 0, -30);
  lv_obj_add_flag(s_logo_container, LV_OBJ_FLAG_HIDDEN);

  // ── Prev/Next buttons ──
  auto make_media_btn = [&](bool is_next) -> lv_obj_t * {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, 44, 44);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(btn, LV_ALIGN_CENTER, is_next ? 90 : -90, -30);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &geist_regular_22, LV_PART_MAIN);
    lv_label_set_text(lbl, is_next ? LV_SYMBOL_NEXT : LV_SYMBOL_PREV);
    lv_obj_center(lbl);
    return btn;
  };

  s_btn_prev = make_media_btn(false);
  s_btn_next = make_media_btn(true);
  lv_obj_add_event_cb(s_btn_prev, on_prev_tap, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(s_btn_next, on_next_tap, LV_EVENT_CLICKED, nullptr);

  s_img_logo = lv_image_create(s_logo_container);
  lv_obj_set_size(s_img_logo, 120, 120);
  lv_image_set_inner_align(s_img_logo, LV_IMAGE_ALIGN_CENTER);
  lv_obj_set_pos(s_img_logo, 0, 0);

  // ── Clock ──
  s_lbl_clock = lv_label_create(parent);
  lv_obj_set_style_text_color(s_lbl_clock, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_lbl_clock, &geist_medium_52, LV_PART_MAIN);
  lv_obj_set_style_text_opa(s_lbl_clock, LV_OPA_90, LV_PART_MAIN);
  lv_label_set_text(s_lbl_clock, "00:00");
  lv_obj_align(s_lbl_clock, LV_ALIGN_CENTER, 0, -20);

  // ── Title (main label — mode name, station, user, playlist, or track) ──
  s_lbl_title = lv_label_create(parent);
  lv_obj_set_style_text_color(s_lbl_title, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_lbl_title, &geist_medium_28, LV_PART_MAIN);
  lv_obj_set_width(s_lbl_title, LCD_H_RES - 80);
  lv_label_set_long_mode(s_lbl_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_align(s_lbl_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_text(s_lbl_title, "Radio");
  lv_obj_align(s_lbl_title, LV_ALIGN_CENTER, 0, 0);

  // ── Subtitle ──
  s_lbl_subtitle = lv_label_create(parent);
  lv_obj_set_style_text_color(s_lbl_subtitle, lv_color_hex(0xAAAAAA),
                              LV_PART_MAIN);
  lv_obj_set_style_text_font(s_lbl_subtitle, &geist_regular_16, LV_PART_MAIN);
  lv_label_set_text(s_lbl_subtitle, "Tap to select");
  lv_obj_align(s_lbl_subtitle, LV_ALIGN_CENTER, 0, 94);

  // ── Position indicator ──
  s_lbl_position = lv_label_create(parent);
  lv_obj_set_style_text_color(s_lbl_position, lv_color_hex(0x888888),
                              LV_PART_MAIN);
  lv_obj_set_style_text_font(s_lbl_position, &geist_regular_16, LV_PART_MAIN);
  lv_label_set_text(s_lbl_position, "");
  lv_obj_align(s_lbl_position, LV_ALIGN_CENTER, 0, 116);
  lv_obj_add_flag(s_lbl_position, LV_OBJ_FLAG_HIDDEN);

  // ── Speaker name ──
  s_lbl_speaker = lv_label_create(parent);
  lv_obj_set_style_text_color(s_lbl_speaker, COL_TEXT_SEC, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_lbl_speaker, &geist_regular_16, LV_PART_MAIN);
  lv_obj_set_style_text_opa(s_lbl_speaker, LV_OPA_60, LV_PART_MAIN);
  lv_label_set_text(s_lbl_speaker, "");
  lv_obj_align(s_lbl_speaker, LV_ALIGN_BOTTOM_MID, 0, -36);
}

static void home_page_destroy() { s_home = nullptr; }

static const PageDef s_home_page = {
    .id = "home",
    .build = home_page_build,
    .destroy = home_page_destroy,
    .tick = nullptr,
};

// ─── Main Screen ────────────────────────────────────────────────────────────

static void build_main_screen() {
  s_screen = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_screen, COL_BG, LV_PART_MAIN);
  lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(s_screen, LCD_H_RES, LCD_V_RES);
  lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_screen, on_screen_pressed, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(s_screen, on_screen_released, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(s_screen, on_screen_gesture, LV_EVENT_GESTURE, nullptr);

  pages_init(s_screen, &s_home_page, on_page_changed);

  // ── WiFi dot ──
  s_wifi_dot = lv_obj_create(s_screen);
  lv_obj_set_size(s_wifi_dot, 8, 8);
  lv_obj_set_style_radius(s_wifi_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_wifi_dot, COL_TEXT_SEC, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_wifi_dot, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_wifi_dot, 0, LV_PART_MAIN);
  lv_obj_remove_flag(s_wifi_dot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(s_wifi_dot, LV_ALIGN_TOP_MID, 0, 36);
}

// ─── Speaker Picker ─────────────────────────────────────────────────────────

static void on_speaker_tap(lv_event_t *e) {
  auto index =
      static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  if (index < 0 || index >= s_discovered.count)
    return;

  auto &speaker = s_discovered.speakers[index];
  sonos_set_speaker(speaker.ip, speaker.port);
  settings_set_speaker_name(speaker.name);
  sonos_start();
  lv_label_set_text(s_lbl_speaker, speaker.name);

  s_on_picker = false;
  lv_screen_load_anim(s_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
  ESP_LOGI(TAG, "Speaker selected: %s (%s)", speaker.name, speaker.ip);
}

static void on_skip_tap(lv_event_t *) {
  s_on_picker = false;
  lv_label_set_text(s_lbl_speaker, "No speaker");
  lv_screen_load_anim(s_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
  ESP_LOGI(TAG, "Speaker picker skipped");
}

static void highlight_picker_item(int highlight) {
  int count = lv_obj_get_child_count(s_scr_speaker_picker);
  for (int i = 1; i < count; i++) {
    lv_obj_t *child = lv_obj_get_child(s_scr_speaker_picker, i);
    if (i - 1 == highlight) {
      lv_obj_set_style_border_color(child, COL_ACCENT, LV_PART_MAIN);
      lv_obj_set_style_border_width(child, 2, LV_PART_MAIN);
      lv_obj_scroll_to_view(child, LV_ANIM_ON);
    } else {
      lv_obj_set_style_border_color(child, COL_ARC_BG, LV_PART_MAIN);
      lv_obj_set_style_border_width(child, 1, LV_PART_MAIN);
    }
  }
}

static void rebuild_speaker_list() {
  if (s_scr_speaker_picker)
    lv_obj_delete(s_scr_speaker_picker);

  s_scr_speaker_picker = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(s_scr_speaker_picker, COL_BG, LV_PART_MAIN);
  lv_obj_set_size(s_scr_speaker_picker, LCD_H_RES, LCD_V_RES);
  lv_obj_set_style_pad_top(s_scr_speaker_picker, 50, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(s_scr_speaker_picker, 50, LV_PART_MAIN);
  lv_obj_set_style_pad_left(s_scr_speaker_picker, 40, LV_PART_MAIN);
  lv_obj_set_style_pad_right(s_scr_speaker_picker, 40, LV_PART_MAIN);
  lv_obj_set_flex_flow(s_scr_speaker_picker, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_scr_speaker_picker, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(s_scr_speaker_picker, 8, LV_PART_MAIN);

  lv_obj_t *title = lv_label_create(s_scr_speaker_picker);
  lv_obj_set_style_text_color(title, COL_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &geist_regular_22, LV_PART_MAIN);
  lv_label_set_text(title, "Select Speaker");

  for (int i = 0; i < s_discovered.count; i++) {
    lv_obj_t *btn = lv_obj_create(s_scr_speaker_picker);
    lv_obj_set_size(btn, LCD_H_RES - 100, 52);
    lv_obj_set_style_bg_color(btn, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, COL_ARC_BG, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(
        btn, COL_ACCENT,
        static_cast<lv_style_selector_t>(static_cast<int>(LV_PART_MAIN) | static_cast<int>(LV_STATE_PRESSED)));

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_color(lbl, COL_TEXT, LV_PART_MAIN);
    lv_label_set_text(lbl, s_discovered.speakers[i].name);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, on_speaker_tap, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(i)));
  }

  if (s_discovered.count == 0) {
    lv_obj_t *lbl = lv_label_create(s_scr_speaker_picker);
    lv_obj_set_style_text_color(lbl, COL_TEXT_SEC, LV_PART_MAIN);
    lv_label_set_text(lbl, "No speakers found.\nCheck your network.");
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  }

  lv_obj_t *skip_btn = lv_obj_create(s_scr_speaker_picker);
  lv_obj_set_size(skip_btn, LCD_H_RES - 100, 48);
  lv_obj_set_style_bg_color(skip_btn, COL_BG, LV_PART_MAIN);
  lv_obj_set_style_border_color(skip_btn, COL_TEXT_SEC, LV_PART_MAIN);
  lv_obj_set_style_border_width(skip_btn, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(skip_btn, 12, LV_PART_MAIN);
  lv_obj_remove_flag(skip_btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(skip_btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(
      skip_btn, COL_ACCENT,
      static_cast<lv_style_selector_t>(static_cast<int>(LV_PART_MAIN) | static_cast<int>(LV_STATE_PRESSED)));

  lv_obj_t *skip_lbl = lv_label_create(skip_btn);
  lv_obj_set_style_text_color(skip_lbl, COL_TEXT_SEC, LV_PART_MAIN);
  lv_label_set_text(skip_lbl, "Skip");
  lv_obj_center(skip_lbl);

  lv_obj_add_event_cb(skip_btn, on_skip_tap, LV_EVENT_CLICKED, nullptr);
}

// ─── Scanning Overlay ───────────────────────────────────────────────────────

static void build_scanning_overlay() {
  s_scanning_overlay = lv_obj_create(s_screen);
  lv_obj_set_size(s_scanning_overlay, LCD_H_RES, LCD_V_RES);
  lv_obj_set_style_bg_color(s_scanning_overlay, COL_BG, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_scanning_overlay, LV_OPA_90, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_scanning_overlay, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(s_scanning_overlay, 0, LV_PART_MAIN);
  lv_obj_remove_flag(s_scanning_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(s_scanning_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_scanning_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(s_scanning_overlay, LV_ALIGN_CENTER, 0, 0);

  s_lbl_scanning = lv_label_create(s_scanning_overlay);
  lv_obj_set_style_text_color(s_lbl_scanning, COL_TEXT_SEC, LV_PART_MAIN);
  lv_label_set_text(s_lbl_scanning, "Scanning\xe2\x80\xa6");
  lv_obj_center(s_lbl_scanning);
}

// ─── Page Changed Callback ──────────────────────────────────────────────────

static void on_page_changed(int index, const char *id) {
  (void)id;
  if (index == 0) {
    lv_anim_delete(s_vol_arc, anim_vol_arc_cb);
    lv_arc_set_value(s_vol_arc, s_volume);
    s_arc_display_val = s_volume;
    update_screen_content();
    show_idle_ui(should_idle());
  }
}

// ─── Album Art ──────────────────────────────────────────────────────────────

static void art_free_pixels() {
  if (s_art_pixels) {
    heap_caps_free(s_art_pixels);
    s_art_pixels = nullptr;
  }
  s_art_dsc = {};
}

// ─── Public API ─────────────────────────────────────────────────────────────

void ui_init() {
  lv_display_t *disp = nullptr;
  lv_indev_t *touch = nullptr;
  display_init(&disp, &touch);

  s_volume = settings_get_volume();

  if (display_lock(200)) {
    build_main_screen();
    build_scanning_overlay();

    s_vol_hide_timer = lv_timer_create(on_vol_hide, VOL_DISPLAY_MS, nullptr);
    lv_timer_pause(s_vol_hide_timer);

    s_browse_timer =
        lv_timer_create(on_browse_timeout, BROWSE_TIMEOUT_MS, nullptr);
    lv_timer_pause(s_browse_timer);

    s_clock_timer = lv_timer_create(on_clock_tick, 30000, nullptr);

    s_bl_timer =
        lv_timer_create(on_backlight_step, BACKLIGHT_FADE_STEP_MS, nullptr);
    lv_timer_pause(s_bl_timer);

    s_bl_inactivity_timer =
        lv_timer_create(on_bl_inactivity, BACKLIGHT_INACTIVITY_MS, nullptr);
    lv_timer_pause(s_bl_inactivity_timer);

    s_press_timer = lv_timer_create(on_press_timer, LONG_PRESS_MS, nullptr);
    lv_timer_pause(s_press_timer);

    lv_timer_create(on_encoder_poll, ENCODER_POLL_MS, nullptr);

    voice_ui_build(s_screen);

    show_idle_ui(true);

    char saved_name[64] = {};
    settings_get_speaker_name(saved_name, sizeof(saved_name));
    if (saved_name[0])
      lv_label_set_text(s_lbl_speaker, saved_name);

    update_screen_content();
    lv_screen_load(s_screen);
    display_unlock();
  }

  ESP_LOGI(TAG, "UI ready");
}

void ui_set_volume(int level) {
  if (display_lock(200)) {
    if (lv_tick_elaps(s_local_vol_ms) < VOL_LOCAL_GRACE_MS) {
      display_unlock();
      return;
    }
    if (level != s_volume) {
      if (pages_is_home() && s_screen_state == AppScreen::NowPlaying) {
        show_volume(level);
      } else {
        lv_anim_delete(s_vol_arc, anim_vol_arc_cb);
        s_arc_display_val = level;
        lv_arc_set_value(s_vol_arc, level);
      }
      s_volume = level;
    }
    display_unlock();
  }
}

void ui_set_play_state(PlayState state) {
  if (state == s_play_state)
    return;
  if (display_lock(50)) {
    s_play_state = state;
    if (s_screen_state == AppScreen::NowPlaying) {
      update_screen_content();
    }
    if (state == PlayState::Stopped &&
        s_screen_state == AppScreen::NowPlaying) {
      transition_to(AppScreen::ModeSelect);
    }
    if (should_idle())
      show_idle_ui(true);
    display_unlock();
  }
}

void ui_set_station(int index) {
  if (index < 0 || index >= RADIO_STATION_COUNT)
    return;
  if (display_lock(50)) {
    s_radio_index = index;
    s_selected_mode = JukeboxMode::Radio;
    if (s_screen_state == AppScreen::NowPlaying) {
      s_external_playing = false;
      s_media = {};
      update_screen_content();
    }
    display_unlock();
  }
}

void ui_set_media_info(const MediaInfo *info) {
  // Download + decode album art OUTSIDE the display lock
  bool art_ready = false;
  if (info && info->has_media && info->art_url[0] &&
      strcmp(info->art_url, s_art_last_url) != 0) {
    ESP_LOGI(TAG, "Art URL changed: %.120s", info->art_url);
    if (!s_art_jpeg)
      s_art_jpeg = static_cast<uint8_t *>(
          heap_caps_malloc(ART_BUF_SIZE, MALLOC_CAP_SPIRAM));
    if (s_art_jpeg) {
      int len = sonos_fetch_art(info->art_url, s_art_jpeg, ART_BUF_SIZE);
      if (len > 0) {
        art_free_pixels();
        uint8_t *px = nullptr;
        int aw = 0, ah = 0;
        bool decoded = art_decode_jpeg(s_art_jpeg, len, &px, &aw, &ah, 160);
        if (decoded) {
          s_art_pixels = px;
          s_art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
          s_art_dsc.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
          s_art_dsc.header.w = static_cast<uint32_t>(aw);
          s_art_dsc.header.h = static_cast<uint32_t>(ah);
          s_art_dsc.header.stride = static_cast<uint32_t>(aw * 2);
          s_art_dsc.data_size = static_cast<uint32_t>(aw * ah * 2);
          s_art_dsc.data = s_art_pixels;
          art_ready = true;
        }
        strncpy(s_art_last_url, info->art_url, sizeof(s_art_last_url) - 1);
        s_art_last_url[sizeof(s_art_last_url) - 1] = '\0';
      }
    }
  }

  if (!display_lock(50))
    return;

  if (info && info->has_media && s_play_state != PlayState::Stopped) {
    s_media = *info;
    s_external_playing = true;

    if (s_screen_state == AppScreen::NowPlaying) {
      update_screen_content();

      // Show album art if available
      if (s_art_pixels) {
        lv_image_set_src(s_img_logo, &s_art_dsc);
        lv_image_set_inner_align(s_img_logo, LV_IMAGE_ALIGN_STRETCH);
        lv_obj_remove_flag(s_logo_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(s_logo_container, LV_OPA_COVER, LV_PART_MAIN);
      }
    }
  } else if (s_external_playing) {
    s_external_playing = false;
    s_media = {};
    s_art_last_url[0] = '\0';
    if (s_screen_state == AppScreen::NowPlaying) {
      update_screen_content();
    }
  }

  display_unlock();
}

void ui_set_wifi_status(bool connected) {
  if (display_lock(50)) {
    lv_obj_set_style_bg_color(s_wifi_dot,
                              connected ? COL_GREEN : COL_TEXT_SEC,
                              LV_PART_MAIN);
    display_unlock();
  }
}

void ui_set_speaker_name(const char *name) {
  if (display_lock(50)) {
    lv_label_set_text(s_lbl_speaker, name);
    display_unlock();
  }
}

// ─── Voice Mode (public) ───────────────────────────────────────────────────

void ui_voice_activate() {
  if (!display_lock(50))
    return;
  activate_voice();
  display_unlock();
}

void ui_voice_deactivate() {
  if (!display_lock(50))
    return;
  deactivate_voice();
  display_unlock();
}

void ui_voice_set_state(VoiceState state) {
  if (!display_lock(50))
    return;
  voice_ui_set_state(state);
  display_unlock();
}

void ui_voice_set_transcript(const char *text, bool is_user) {
  if (!display_lock(50))
    return;
  voice_ui_set_transcript(text, is_user);
  display_unlock();
}

bool ui_is_voice_active() { return s_voice_active; }

// ─── Speaker Picker / Scanning (public) ─────────────────────────────────────

void ui_show_scanning() {
  if (display_lock(50)) {
    lv_obj_remove_flag(s_scanning_overlay, LV_OBJ_FLAG_HIDDEN);
    display_unlock();
  }
}

void ui_show_speaker_picker(const DiscoveryResult *speakers) {
  if (display_lock(200)) {
    lv_obj_add_flag(s_scanning_overlay, LV_OBJ_FLAG_HIDDEN);
    memcpy(&s_discovered, speakers, sizeof(s_discovered));
    s_speaker_highlight = 0;
    rebuild_speaker_list();
    s_on_picker = true;
    lv_screen_load_anim(s_scr_speaker_picker, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200,
                        0, false);
    display_unlock();
  }
}
