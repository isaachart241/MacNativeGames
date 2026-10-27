#pragma once
#include "runtime.h"

/* Tears of the Kingdom abilities in Breath of the Wild 1.6.0 (botw_abilities.c). Off unless TOTK_ABILITIES=1 (or the
   launcher's defaults key TOTK_ABILITIES); Breath of the Wild 1.9.0 and Tears of the Kingdom builds get the empty stubs. */
void botw_abilities_init(Module *main_module);   /* runtime_game_patches, after the code patches */
uint64_t botw_abilities_filter_buttons(uint64_t buttons);   /* input.c: the buttons the game gets (Ascend takes ZL + L) */
void botw_abilities_tick(void);                   /* every present, on the game's thread (runtime_frame_step_update) */
