#ifndef VERSION_H
#define VERSION_H

// Semantic version components (populated by CMake from git tag)
#ifndef VERSION_MAJOR
#define VERSION_MAJOR 0
#endif

#ifndef VERSION_MINOR
#define VERSION_MINOR 0
#endif

#ifndef VERSION_REVISION
#define VERSION_REVISION 0
#endif

// Git revision (populated by CMake at build time)
#ifndef GIT_REVISION
#define GIT_REVISION "unknown"
#endif

// Release flag (populated by CMake at build time)
#ifndef IS_RELEASE_BUILD
#define IS_RELEASE_BUILD 0
#endif

// Get formatted version string
const char* get_version_string(void);

#endif
