idf_component_register(
    SRCS
        "sendspin_iface.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_event
        esp_timer
        esp_wifi
        nvs_flash
    PRIV_REQUIRES
        sendspin__sendspin-cpp
        espressif__mdns
)
