idf_component_register(
    SRCS "main.cpp"
    REQUIRES common protocol remote_core esp_wifi nvs_flash esp_timer esp_event esp_netif driver)
