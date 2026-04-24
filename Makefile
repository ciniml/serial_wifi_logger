IDF_PATH    ?= $(HOME)/esp-idf/6.0
IDF_EXPORTS  = $(IDF_PATH)/export.sh
SDKCONFIG_QEMU = sdkconfig.defaults;sdkconfig.qemu

.PHONY: build build-qemu qemu clean

build:
	bash -c "source $(IDF_EXPORTS) && idf.py build"

build-qemu:
	bash -c "source $(IDF_EXPORTS) && idf.py -DSDKCONFIG_DEFAULTS='$(SDKCONFIG_QEMU)' build"

qemu: build-qemu
	bash -c "source $(IDF_EXPORTS) && idf.py -DSDKCONFIG_DEFAULTS='$(SDKCONFIG_QEMU)' qemu"

clean:
	bash -c "source $(IDF_EXPORTS) && idf.py fullclean"
