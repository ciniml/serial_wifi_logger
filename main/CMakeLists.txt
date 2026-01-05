idf_component_register(SRCS usb_serial_auto_main.c
                            provisioning.c
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES espressif__usb_host_cdc_acm
                                  usb_host_ftdi_sio
                                  esp_wifi
                                  lwip
                                  nvs_flash
                    )
