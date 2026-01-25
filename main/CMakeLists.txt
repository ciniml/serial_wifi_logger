# Get git version from tag (e.g., v1.0.0)
execute_process(
    COMMAND git describe --tags --abbrev=0
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

# Parse semantic version from tag
# Supports v1.0.0, 1.0.0, v1.0 (revision defaults to 0), v1 (minor and revision default to 0)
if(GIT_TAG MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    # Full version: v1.0.0
    set(VERSION_MAJOR ${CMAKE_MATCH_1})
    set(VERSION_MINOR ${CMAKE_MATCH_2})
    set(VERSION_REVISION ${CMAKE_MATCH_3})
elseif(GIT_TAG MATCHES "^v?([0-9]+)\\.([0-9]+)")
    # Partial version: v1.0 (revision defaults to 0)
    set(VERSION_MAJOR ${CMAKE_MATCH_1})
    set(VERSION_MINOR ${CMAKE_MATCH_2})
    set(VERSION_REVISION 0)
elseif(GIT_TAG MATCHES "^v?([0-9]+)")
    # Major only: v1 (minor and revision default to 0)
    set(VERSION_MAJOR ${CMAKE_MATCH_1})
    set(VERSION_MINOR 0)
    set(VERSION_REVISION 0)
else()
    # Fallback to 0.0.0 if no valid tag found
    set(VERSION_MAJOR 0)
    set(VERSION_MINOR 0)
    set(VERSION_REVISION 0)
endif()

# Get git revision (short commit hash)
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

# Check for dirty working tree (uncommitted changes)
execute_process(
    COMMAND git diff-index --quiet HEAD --
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RESULT_VARIABLE GIT_IS_DIRTY
    ERROR_QUIET
)

# Append -dirty suffix if working tree is dirty
if(GIT_IS_DIRTY)
    set(GIT_REVISION "${GIT_REVISION}-dirty")
endif()

# Fallback if git command fails
if(NOT GIT_REVISION)
    set(GIT_REVISION "unknown")
endif()

# Detect CI build
if(DEFINED ENV{CI})
    set(IS_RELEASE_BUILD 1)
else()
    set(IS_RELEASE_BUILD 0)
endif()

idf_component_register(SRCS main.c
                            provisioning.c
                            version.c
                            ota_server.c
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES espressif__usb_host_cdc_acm
                                  usb_host_ftdi_sio
                                  espressif__mdns
                                  esp_wifi
                                  lwip
                                  nvs_flash
                                  esp_http_server
                                  app_update
                    )

# Add compile definitions for version information
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    VERSION_MAJOR=${VERSION_MAJOR}
    VERSION_MINOR=${VERSION_MINOR}
    VERSION_REVISION=${VERSION_REVISION}
    GIT_REVISION="${GIT_REVISION}"
    IS_RELEASE_BUILD=${IS_RELEASE_BUILD}
)
