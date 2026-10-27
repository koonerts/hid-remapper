#ifndef _GW_HOSTCAP_H_
#define _GW_HOSTCAP_H_

#include <stdint.h>

// v23: what the host on the Feather's USB-C side asks for while it enumerates the Feather (gw_hostcap.cc).
// The ATEN switch puts a different host there (a PC, the XIM, the spare Feather in front of the PS5), and each
// asks in its own order and sizes, so this is how the Feather can tell them apart.

#define GW_HOSTCAP_MAX 29  // requests kept per enumeration (status bytes 272-503)

// status bytes 256-511 (layout in gw_hostcap.cc)
void gw_hostcap_fill(uint8_t* s);
uint8_t gw_hostcap_id();   // v26
uint8_t gw_hostcap_upd();  // v26

// changes when there is something new to read in 256-511: at most every 300 ms, and once more after a burst
uint8_t gw_hostcap_seq();
uint16_t gw_hostcap_first_desc_len();                  // v35: wLength of the capture's first device descriptor request (0 none)
bool gw_hostcap_cfg_ago(uint32_t now, uint32_t* us);  // v35: the capture's SET_CONFIGURATION came, us ago

#endif
