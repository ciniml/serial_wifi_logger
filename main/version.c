#include "version.h"
#include <stdio.h>

// Static buffer for version string
static char version_buffer[64];

const char* get_version_string(void) {
    snprintf(version_buffer, sizeof(version_buffer),
             "%d.%d.%d g%s %s",
             VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION,
             GIT_REVISION,
             IS_RELEASE_BUILD ? "RELEASE" : "DEV");
    return version_buffer;
}
