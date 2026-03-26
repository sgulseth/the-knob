set(SRCS
    "main.cpp"
    "fonts/geist_regular_18.c"
    "fonts/geist_regular_16.c"
    "fonts/geist_regular_22.c"
    "fonts/geist_medium_28.c"
    "fonts/geist_medium_52.c"
    "ui/ui.cpp"
    "ui/ui_timer.cpp"
    "ui/ui_voice.cpp"
    "ui/ui_settings.cpp"
    "ui/images/nrk_p2.c"
    "ui/images/nrk_nyheter.c"
    "voice/tools_radio.cpp"
)

set(INCLUDE_DIRS
    "."
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    REQUIRES
        knob_hal
        knob_net
        knob_storage
        knob_ui
        knob_voice
        knob_sonos
        knob_timer
        esp_event
        esp_timer
        esp_http_client
        esp_wifi
        nvs_flash
    PRIV_REQUIRES
        esp_netif
        esp_psram
)

target_compile_options(${COMPONENT_LIB} PUBLIC
    -std=gnu++20
    -fno-exceptions
    -fno-rtti
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
