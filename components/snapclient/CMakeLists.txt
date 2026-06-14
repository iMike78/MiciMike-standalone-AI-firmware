idf_component_register(
    SRCS
        "snapclient.c"
        "snap_proto.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        lwip
        esp_netif
        esp_event
        esp_timer
        # FLAC + PCM decoding will land via this managed component in iter 3:
        espressif__esp_audio_codec
)
