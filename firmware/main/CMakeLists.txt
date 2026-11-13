idf_component_register(
    SRCS "main.c" "proto.c" "usb_link.c" "meter.c" "bridge.c" "mcp.c" "http.c" "scpi_server.c"
         "net.c" "captive_dns.c" "portal.c" "board.c" "console.c"
    INCLUDE_DIRS "."
    # The dashboard is the prototype's own page, so both serve the same file.
    EMBED_TXTFILES "../../bridge/static/index.html" "setup.html"
    REQUIRES esp_http_server esp_wifi esp_netif nvs_flash json esp_driver_gpio esp_driver_uart
             esp_timer app_update esp_app_format usb lwip)
# Copies into fixed-size fields truncate on purpose (meter replies are short; anything longer is junk).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
