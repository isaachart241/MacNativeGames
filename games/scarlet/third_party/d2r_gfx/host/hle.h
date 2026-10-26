#pragma once
#include "dm.h"
typedef struct {const char *name;void *fn;const char *why;} hle_force_ent;
#ifdef __APPLE__   /* Android: an ELF section of the same name (runtime/gfx_bridge.c reads both the same way) */
#define HLE_SECTION section("__DATA,totkgfx")
#else
#define HLE_SECTION section("totkgfx")
#endif
#define HLE_FORCE(mangled,reason,ret,cname,...) static ret cname(__VA_ARGS__); __attribute__((used,HLE_SECTION)) static const hle_force_ent ent_##cname={mangled,(void *)cname,reason}; static ret cname(__VA_ARGS__)
