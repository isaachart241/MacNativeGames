/* Thor port, stage T1: no controller yet (the device's built-in pad arrives with the input stage). The game sees the
   keyboard-script input of runtime/input.c (TOTK_INPUT_FILE) only; rumble is accepted and dropped. */
#include <string.h>
#include "runtime.h"

void runtime_gamepad_start(void) {}
void runtime_gamepad_clear(void) {}
void runtime_gamepad_read(RuntimeGamepadState *out) { memset(out, 0, sizeof *out); }
void runtime_rumble_set(unsigned side, float al, float fl, float ah, float fh) { (void)side; (void)al; (void)fl; (void)ah; (void)fh; }
