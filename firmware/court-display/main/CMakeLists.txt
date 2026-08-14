idf_component_register(
    SRCS "main.cpp" "board_7b.cpp" "storage.cpp" "buzzer.cpp"
    INCLUDE_DIRS "."
    REQUIRES common domain protocol application persistence ui sound
             esp_wifi nvs_flash esp_timer esp_event esp_netif driver esp_lcd
    # The club list travels inside the binary: no LittleFS provisioning step,
    # and flashing it cannot disturb the journal sharing that partition.
    EMBED_TXTFILES "${CMAKE_CURRENT_LIST_DIR}/../../../config/players.txt")
