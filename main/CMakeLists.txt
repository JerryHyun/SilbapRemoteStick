idf_build_get_property(python PYTHON)
set(WEB_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/../web)
set(WEB_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/web)
# Keep in sync with FILES in tools/embed_web.py and the ASSET()/ASSETS[] table in main/web.c
# (test/host/test_web_assets.py checks all three).
set(WEB_NAMES index.html setup.html app.js app.css i18n/ko.json i18n/en.json i18n/zh.json i18n/ja.json)
set(WEB_SOURCES "")
set(WEB_GZ "")
foreach(n ${WEB_NAMES})
  list(APPEND WEB_SOURCES ${WEB_SRC_DIR}/${n})
  list(APPEND WEB_GZ ${WEB_OUT_DIR}/${n}.gz)
endforeach()
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
add_custom_command(OUTPUT ${WEB_GZ}
    COMMAND ${python} ${CMAKE_CURRENT_LIST_DIR}/../tools/embed_web.py ${WEB_SRC_DIR} ${WEB_OUT_DIR}
    DEPENDS ${WEB_SOURCES} ${CMAKE_CURRENT_LIST_DIR}/../tools/embed_web.py
    COMMENT "Compressing web/ for embedding" VERBATIM)
add_custom_target(web_gz DEPENDS ${WEB_GZ})
endif()

idf_component_register(SRCS main.c button.c button_policy.c led.c refresh.c refresh_policy.c sdcard.c partition_card.c settings.c timekeep.c timekeep_core.c netpolicy.c netif.c sdlog.c usb_msc.c web.c web_decide.c webdav.c webdav_xml.c fsops.c fsops_esp.c api_files.c api_list_core.c api_settings.c settings_json.c setup_apply.c provision.c ssid_list.c dns_reply.c fs_names.c esp_lcd_st7735.c lcd.c font_data.c font_lookup.c lcd_view.c lcd_ui.c INCLUDE_DIRS .
    EMBED_FILES ${WEB_GZ}
    REQUIRES webcore namemap fatfs sdmmc esp_driver_sdmmc nvs_flash esp_tinyusb esp_timer esp_hw_support esp_driver_gpio freertos esp_wifi esp_netif esp_event mdns esp_system heap bootloader_support esp_http_server esp_app_format setupfile esp_lcd esp_driver_spi)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
  add_dependencies(${COMPONENT_LIB} web_gz)
endif()
