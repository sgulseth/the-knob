#pragma once

#include "app_config.h"
#include "discovery.h"

void ui_init();

void ui_set_volume(int level);
void ui_set_play_state(PlayState state);
void ui_set_media_info(const MediaInfo *info);
void ui_set_station(int index);
void ui_set_wifi_status(bool connected);
void ui_set_speaker_name(const char *name);

void ui_show_speaker_picker(const DiscoveryResult *speakers);
void ui_show_scanning();

void ui_voice_activate();
void ui_voice_deactivate();
void ui_voice_set_state(VoiceState state);
void ui_voice_set_transcript(const char *text, bool is_user);
bool ui_is_voice_active();
