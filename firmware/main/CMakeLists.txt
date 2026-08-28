idf_component_register(SRCS "main.c" "app_camera.c" "hw2d.c" "hw2d_pie.S"
                            "ui_beauty_camera.c" "pvc_power.c" "pvc_clock.c"
                            "pvc_jpeg.c" "pvc_sound.c"
                            "net/pvc_net.c" "net/pvc_store.c" "net/pvc_http.c"
                            "net/pvc_pair.c" "net/pvc_upload.c" "net/pvc_prov.c"
                            "net/pvc_feed.c" "net/pvc_config.c"
                       INCLUDE_DIRS "." "net"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash
                                     esp_http_client esp-tls mbedtls json esp_timer
                                     wifi_provisioning bt driver esp_driver_i2c)
