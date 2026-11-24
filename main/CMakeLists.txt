idf_component_register(
    SRCS "main.c" "network.c" "flights.c" "weather.c" "screen.c" "note4c.c" "qr/qrcodegen.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "setup.html"
    PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_adc esp_wifi esp_netif esp_event
        esp_http_client esp_http_server mbedtls nvs_flash lwip json esp_timer esp_psram
)
