# Only build FTDI driver when selected in Kconfig
if(CONFIG_USB_SERIAL_DRIVER_FTDI)
    idf_component_register(
        SRCS
            "src/ftdi_host_protocol.c"
            "src/ftdi_host_descriptor_parsing.c"
            "src/ftdi_sio_host.c"
            "src/ftdi_sio_host_ops.c"
        INCLUDE_DIRS
            "include"
        PRIV_INCLUDE_DIRS
            "private_include"
        REQUIRES
            usb
        PRIV_REQUIRES
            esp_system
    )
else()
    # Register empty component when not selected
    idf_component_register()
endif()
