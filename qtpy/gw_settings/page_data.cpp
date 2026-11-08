// 11-07: the settings page's gzipped bytes in their own file, so a page edit recompiles only this (a few seconds) and not
// the whole sketch (gw_settings.ino, ~20 s). page_gz.h is made from page.h by page_gz.ps1 (qtpy-build.ps1 runs it).
#include <Arduino.h>
#include "page_gz.h"

extern const uint8_t* const gw_page_gz = PAGE_GZ;
extern const size_t gw_page_gz_len = PAGE_GZ_LEN;
extern const uint32_t gw_page_src_fnv = PAGE_SRC_FNV;