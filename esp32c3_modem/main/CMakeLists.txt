idf_component_register(SRCS "modem.c" "lcd.c"
                       PRIV_REQUIRES esp_wifi nvs_flash esp_driver_usb_serial_jtag esp_driver_uart esp_timer esp_lcd esp_driver_spi esp_driver_gpio)
# modem.c replaces the Wi-Fi library's raw-frame filter (ieee80211_raw_frame_sanity_check), which rejects the
# auth and assoc responses a PictoChat host must send. Our definition is linked first and wins.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-zmuldefs")
