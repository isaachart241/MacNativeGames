/* Thor port: getsectiondata reads this executable's ELF section table (/proc/self/exe); the segment name is ignored,
   as ELF sections carry only the section name (nvn_impl.h NVN_SECTION, gfx_bridge.c GFX_SECTION). */
#pragma once
#include <stdint.h>
struct mach_header_64 { uint32_t magic; };
extern const struct mach_header_64 _mh_execute_header;
uint8_t *getsectiondata(const struct mach_header_64 *mh, const char *segname, const char *sectname, unsigned long *size);
