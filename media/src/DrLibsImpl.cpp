// Single translation unit holding the vendored dr_libs implementations (public domain / MIT-0).
// DR_MP3_NO_STDIO / DR_FLAC_NO_STDIO come from media/CMakeLists.txt: decoding is memory-only.
#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>

#define DR_FLAC_IMPLEMENTATION
#include <dr_flac.h>
