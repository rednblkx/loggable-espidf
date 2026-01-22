idf_component_register(
    SRCS "src/loggable_espidf.cpp" "src/loggable_os_freertos.cpp"
    INCLUDE_DIRS "include"
    REQUIRES loggable log freertos
    PRIV_REQUIRES esp_timer
)
