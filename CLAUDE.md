# serial_wifi_logger

## ESP-IDF

Use **ESP-IDF 6.0** located at `~/esp-idf/6.0`.

```bash
source ~/esp-idf/6.0/export.sh
```

## Build

```bash
# Normal build
idf.py build

# QEMU build (use sdkconfig.qemu overrides)
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu" build
```

## QEMU

IDF 6.0 has built-in QEMU support. Run without monitor for direct serial I/O:

```bash
idf.py qemu
```

Run with IDF monitor (requires interactive TTY):

```bash
idf.py qemu monitor
```

Extra QEMU arguments:

```bash
idf.py qemu monitor --qemu-extra-args="<extra args>"
```

No need to merge flash images or locate qemu-system-xtensa manually — `idf.py qemu` handles everything.

The QEMU build requires the QEMU sdkconfig overrides:

```bash
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu" build
idf.py qemu
```
