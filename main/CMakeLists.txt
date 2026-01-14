idf_component_register(SRCS main.c
                            provisioning.c
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES espressif__usb_host_cdc_acm
                                  usb_host_ftdi_sio
                                  espressif__mdns
                                  esp_wifi
                                  lwip
                                  nvs_flash
                    )
