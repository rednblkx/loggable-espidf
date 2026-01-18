idf_component_register(
    SRCS "src/loggable_espidf.cpp"
    INCLUDE_DIRS "include"
    REQUIRES loggable log
    PRIV_REQUIRES esp_timer
)
