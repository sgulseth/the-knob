#include "ui_settings.h"
#include "fonts.h"
#include "ui_pages.h"

#include "app_config.h"
#include "discovery.h"
#include "display.h"
#include "haptic.h"
#include "settings.h"
#include "sonos.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>

static constexpr const char *TAG = "ui_settings";
static constexpr const char *PAGE_ID = "settings";
static constexpr int SETTINGS_PAGE_PRIORITY = 100;

// ─── Palette ────────────────────────────────────────────────────────────────

#define COL_S_BG lv_color_hex(0x000000)
#define COL_S_TEXT lv_color_hex(0xFFFFFF)
#define COL_S_DIM lv_color_hex(0x8E8E93)
#define COL_S_ACCENT lv_color_hex(0x0A84FF)
#define COL_S_ACTIVE lv_color_hex(0x30D158)
#define COL_S_BORDER lv_color_hex(0x2C2C2E)

// ─── State ──────────────────────────────────────────────────────────────────

static bool s_registered = false;
static bool s_scanning = false;

static DiscoveryResult s_speakers = {};
static int s_highlight = 0;

static char s_current_name[64] = {};
static char s_current_ip[40] = {};

// ─── Widgets ────────────────────────────────────────────────────────────────

static lv_obj_t *s_lbl_title;
static lv_obj_t *s_lbl_current;
static lv_obj_t *s_lbl_current_ip;
static lv_obj_t *s_lbl_status;
static lv_obj_t *s_list_container;
static lv_obj_t *s_speaker_items[DISCOVERY_MAX_SPEAKERS];
static int s_speaker_item_count = 0;

// ─── Forward Declarations ───────────────────────────────────────────────────

static void rebuild_speaker_list();
static void highlight_item(int index);

// ─── Speaker List ───────────────────────────────────────────────────────────

static void rebuild_speaker_list() {
  // Clear existing items
  if (s_list_container) {
    lv_obj_clean(s_list_container);
  }
  s_speaker_item_count = 0;

  if (s_scanning) {
    lv_label_set_text(s_lbl_status, "Scanning\xe2\x80\xa6");
    lv_obj_set_style_text_color(s_lbl_status, COL_S_DIM, LV_PART_MAIN);
    lv_obj_remove_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  if (s_speakers.count == 0) {
    lv_label_set_text(s_lbl_status, "No speakers found");
    lv_obj_set_style_text_color(s_lbl_status, COL_S_DIM, LV_PART_MAIN);
    lv_obj_remove_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  lv_obj_add_flag(s_lbl_status, LV_OBJ_FLAG_HIDDEN);

  for (int i = 0; i < s_speakers.count && i < DISCOVERY_MAX_SPEAKERS; i++) {
    lv_obj_t *item = lv_obj_create(s_list_container);
    lv_obj_set_size(item, 220, 40);
    lv_obj_set_style_bg_color(item, COL_S_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(item, COL_S_BORDER, LV_PART_MAIN);
    lv_obj_set_style_border_width(item, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(item, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_left(item, 12, LV_PART_MAIN);
    lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(item, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(item);
    lv_obj_set_style_text_color(lbl, COL_S_TEXT, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &geist_regular_16, LV_PART_MAIN);
    lv_label_set_text(lbl, s_speakers.speakers[i].name);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    // Show green dot for currently active speaker
    if (strcmp(s_speakers.speakers[i].ip, s_current_ip) == 0) {
      lv_obj_t *dot = lv_obj_create(item);
      lv_obj_set_size(dot, 8, 8);
      lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_bg_color(dot, COL_S_ACTIVE, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
      lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -8, 0);
    }

    s_speaker_items[i] = item;
    s_speaker_item_count++;
  }

  if (s_highlight >= s_speaker_item_count)
    s_highlight = 0;
  highlight_item(s_highlight);
}

static void highlight_item(int index) {
  for (int i = 0; i < s_speaker_item_count; i++) {
    if (i == index) {
      lv_obj_set_style_border_color(s_speaker_items[i], COL_S_ACCENT,
                                    LV_PART_MAIN);
      lv_obj_set_style_border_width(s_speaker_items[i], 2, LV_PART_MAIN);
      lv_obj_scroll_to_view(s_speaker_items[i], LV_ANIM_ON);
    } else {
      lv_obj_set_style_border_color(s_speaker_items[i], COL_S_BORDER,
                                    LV_PART_MAIN);
      lv_obj_set_style_border_width(s_speaker_items[i], 1, LV_PART_MAIN);
    }
  }
}

// ─── Discovery Task ─────────────────────────────────────────────────────────

static void scan_task(void *) {
  DiscoveryResult result = {};
  discovery_scan(&result);

  if (display_lock(200)) {
    s_speakers = result;
    s_scanning = false;
    s_highlight = 0;

    // Try to highlight the current speaker
    for (int i = 0; i < result.count; i++) {
      if (strcmp(result.speakers[i].ip, s_current_ip) == 0) {
        s_highlight = i;
        break;
      }
    }

    if (s_registered)
      rebuild_speaker_list();
    display_unlock();
  }

  vTaskDelete(nullptr);
}

static void start_scan() {
  if (s_scanning)
    return;
  s_scanning = true;
  if (s_registered)
    rebuild_speaker_list();
  xTaskCreatePinnedToCore(scan_task, "scan", 6144, nullptr, 3, nullptr, 1);
}

// ─── Select Speaker ─────────────────────────────────────────────────────────

static void select_speaker(int index) {
  if (index < 0 || index >= s_speakers.count)
    return;

  auto &speaker = s_speakers.speakers[index];
  ESP_LOGI(TAG, "Speaker selected: %s (%s:%d)", speaker.name, speaker.ip,
           speaker.port);

  sonos_set_speaker(speaker.ip, speaker.port);
  settings_set_speaker_name(speaker.name);
  settings_set_speaker_ip(speaker.ip);
  sonos_start();

  strncpy(s_current_name, speaker.name, sizeof(s_current_name) - 1);
  strncpy(s_current_ip, speaker.ip, sizeof(s_current_ip) - 1);

  // Update current speaker label
  if (s_lbl_current)
    lv_label_set_text(s_lbl_current, s_current_name);
  if (s_lbl_current_ip)
    lv_label_set_text(s_lbl_current_ip, s_current_ip);

  // Rebuild to update green dot
  rebuild_speaker_list();

  haptic_play(HAPTIC_CLICK);
}

// ─── Page Lifecycle ─────────────────────────────────────────────────────────

static void page_build(lv_obj_t *parent) {
  lv_obj_set_style_bg_color(parent, COL_S_BG, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, LV_PART_MAIN);

  // Title
  s_lbl_title = lv_label_create(parent);
  lv_obj_set_style_text_color(s_lbl_title, COL_S_DIM, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_lbl_title, &geist_regular_16, LV_PART_MAIN);
  lv_label_set_text(s_lbl_title, "Speaker");
  lv_obj_align(s_lbl_title, LV_ALIGN_TOP_MID, 0, 50);

  // Current speaker name
  s_lbl_current = lv_label_create(parent);
  lv_obj_set_style_text_color(s_lbl_current, COL_S_TEXT, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_lbl_current, &geist_medium_28, LV_PART_MAIN);
  lv_obj_set_style_text_align(s_lbl_current, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_set_width(s_lbl_current, 260);
  lv_label_set_long_mode(s_lbl_current, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_label_set_text(s_lbl_current, s_current_name[0] ? s_current_name : "None");
  lv_obj_align(s_lbl_current, LV_ALIGN_TOP_MID, 0, 72);

  // Current speaker IP
  s_lbl_current_ip = lv_label_create(parent);
  lv_obj_set_style_text_color(s_lbl_current_ip, COL_S_DIM, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_lbl_current_ip, &geist_regular_16, LV_PART_MAIN);
  lv_label_set_text(s_lbl_current_ip, s_current_ip[0] ? s_current_ip : "");
  lv_obj_align(s_lbl_current_ip, LV_ALIGN_TOP_MID, 0, 104);

  // Speaker list container (scrollable)
  s_list_container = lv_obj_create(parent);
  lv_obj_set_size(s_list_container, 240, 120);
  lv_obj_align(s_list_container, LV_ALIGN_CENTER, 0, 20);
  lv_obj_set_style_bg_opa(s_list_container, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_list_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_list_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_list_container, 6, LV_PART_MAIN);
  lv_obj_set_flex_flow(s_list_container, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_list_container, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(s_list_container, LV_OBJ_FLAG_CLICKABLE);

  // Status label (shown when scanning or no speakers)
  s_lbl_status = lv_label_create(parent);
  lv_obj_set_style_text_color(s_lbl_status, COL_S_DIM, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_lbl_status, &geist_regular_16, LV_PART_MAIN);
  lv_obj_set_style_text_align(s_lbl_status, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_label_set_text(s_lbl_status, "Turn to browse, tap to select");
  lv_obj_align(s_lbl_status, LV_ALIGN_BOTTOM_MID, 0, -50);

  // Load current speaker info
  settings_get_speaker_name(s_current_name, sizeof(s_current_name));
  settings_get_speaker_ip(s_current_ip, sizeof(s_current_ip));

  // Start scan automatically when page is built
  start_scan();
}

static void page_destroy() {
  s_lbl_title = nullptr;
  s_lbl_current = nullptr;
  s_lbl_current_ip = nullptr;
  s_lbl_status = nullptr;
  s_list_container = nullptr;
  s_speaker_item_count = 0;
}

static uint32_t s_scan_anim_ms = 0;
static int s_scan_dots = 0;

static void page_tick() {
  if (s_scanning && s_lbl_status) {
    uint32_t now = lv_tick_get();
    if (now - s_scan_anim_ms > 500) {
      s_scan_anim_ms = now;
      s_scan_dots = (s_scan_dots + 1) % 4;
      const char *texts[] = {"Scanning", "Scanning.", "Scanning..", "Scanning..."};
      lv_label_set_text(s_lbl_status, texts[s_scan_dots]);
    }
  }
}

static const PageDef s_settings_def = {
    .id = PAGE_ID,
    .build = page_build,
    .destroy = page_destroy,
    .tick = page_tick,
};

// ─── Public API ─────────────────────────────────────────────────────────────

void ui_settings_init() {
  // Load current speaker info
  settings_get_speaker_name(s_current_name, sizeof(s_current_name));
  settings_get_speaker_ip(s_current_ip, sizeof(s_current_ip));

  // Register the settings page — priority 100 puts it to the right of home
  pages_add(&s_settings_def, SETTINGS_PAGE_PRIORITY);
  s_registered = true;
  ESP_LOGI(TAG, "Settings page registered (priority %d)", SETTINGS_PAGE_PRIORITY);
}

void ui_settings_rescan() {
  if (display_lock(50)) {
    start_scan();
    display_unlock();
  }
}

void ui_settings_encoder(int steps) {
  if (s_scanning || s_speaker_item_count == 0)
    return;
  int idx = s_highlight + steps;
  s_highlight = ((idx % s_speaker_item_count) + s_speaker_item_count) %
                s_speaker_item_count;
  highlight_item(s_highlight);
  haptic_play(HAPTIC_TICK);
}

void ui_settings_tap() {
  if (s_scanning)
    return;
  if (s_speaker_item_count == 0) {
    // No speakers — trigger rescan
    start_scan();
    return;
  }
  select_speaker(s_highlight);
}

bool ui_settings_is_active() {
  return s_registered && pages_current_index() == pages_find(PAGE_ID);
}
