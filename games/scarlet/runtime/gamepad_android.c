/* Thor port: the controller. In the APK (stage T3) runtime/activity_android.c hands every input event of the window to
   runtime_android_input; the Thor's built-in pad ("Odin Controller") sends Android gamepad key codes for its buttons
   and joystick motion for its sticks (X / Y, Z / RZ), triggers (BRAKE / GAS) and d-pad (HAT_X / HAT_Y, also sent as
   d-pad keys). They become the same physical buttons as on the Mac (runtime/controller_map.h: face buttons by position,
   so the button on the right is the Switch's A) and go through the player's layout (TOTK_CONTROLS, the modes hold, tap,
   toggle and aim) as in runtime/gamepad.m. The command-line build gets no events: there the game sees the keyboard
   script of runtime/input.c (TOTK_INPUT_FILE) only. Rumble is accepted and dropped. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "gamepad_layout.h"
#include "controller_map.h"
#ifdef SCARLET_APK
#include <android/input.h>
#include <android/keycodes.h>
#endif

static pthread_mutex_t gamepadLock = PTHREAD_MUTEX_INITIALIZER;
static RuntimeGamepadState gamepadState;
static uint64_t pressedEdges;
static int started;
static uint16_t controllerMap[CONTROLLER_BUTTONS];
static uint8_t controllerModes[CONTROLLER_BUTTONS];
static uint64_t physicalPrev;
static uint64_t tapUntil[CONTROLLER_BUTTONS];   /* ns, for tap */
static int toggleOn[CONTROLLER_BUTTONS];        /* for toggle */
static int toggleMoved[CONTROLLER_BUTTONS];     /* the stick moved since the toggle went on */

/* What the pad holds now (under gamepadLock): keys, as physical buttons apart from the face buttons; the face buttons
   by Android's names (A bottom, B right, X left, Y top on the Thor in its Xbox layout); the axes. */
static uint64_t keyButtons;
static int faceA, faceB, faceX, faceY;
static float axisLX, axisLY, axisRX, axisRY, axisLT, axisRT, hatX, hatY;
static int connected, byPosition = -1;

static void load_controller_map(void) {
    controller_map_default(controllerMap, controllerModes);
    const char *path = getenv("TOTK_CONTROLS");
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    if (!f) { fprintf(stderr, "[totk-input] controller layout: default\n"); return; }
    char text[16384]; size_t n = fread(text, 1, sizeof text - 1, f); int whole = feof(f); fclose(f);
    if (!whole) runtime_fail("controller layout file too large");
    text[n] = 0;
    const char *error = NULL; int line = controller_map_parse(text, controllerMap, controllerModes, &error);
    if (line) { fprintf(stderr, "[totk-input] %s line %d: %s\n", path, line, error); runtime_fail("controller layout file"); }
    fprintf(stderr, "[totk-input] controller layout: %s\n", path);
}

static int32_t stick_axis(float value) {
    const float deadzone = 0.12f;
    float magnitude = fabsf(value);
    if (magnitude <= deadzone) return 0;
    if (magnitude > 1.0f) magnitude = 1.0f;
    int32_t result = (int32_t)((magnitude - deadzone) / (1.0f - deadzone) * 32767.0f);
    return value < 0 ? -result : result;
}

/* The game's state from what the pad holds, as refresh_gamepads in runtime/gamepad.m (under gamepadLock). Called at
   each event and at each read, so a tap's pulse ends on time without an event. */
static void refresh_locked(void) {
    RuntimeGamepadState next = {0};
    next.connected = connected;
    uint64_t physical = keyButtons | gamepad_face_positions(faceA, faceB, faceX, faceY, byPosition);
    if (axisLT > 0.5f) physical |= 1ULL << 8;
    if (axisRT > 0.5f) physical |= 1ULL << 9;
    if (hatX < -0.5f) physical |= 1ULL << 12;
    if (hatY < -0.5f) physical |= 1ULL << 13;
    if (hatX > 0.5f) physical |= 1ULL << 14;
    if (hatY > 0.5f) physical |= 1ULL << 15;
    next.lx = stick_axis(axisLX); next.ly = stick_axis(-axisLY);   /* Android's y grows downwards, the Switch's upwards */
    next.rx = stick_axis(axisRX); next.ry = stick_axis(-axisRY);
    uint64_t now = runtime_now_ns(), edges = physical & ~physicalPrev, synthetic = 0, suppressed = 0;
    int moving = next.lx || next.ly;
    physicalPrev = physical;
    for (int i = 0; i < CONTROLLER_BUTTONS; i++) {
        uint64_t bit = 1ULL << i;
        int kind = controller_mode_kind(controllerModes[i]);
        if (kind == CONTROLLER_TAP && (edges & bit)) tapUntil[i] = now + 400000000ULL;
        if (kind == CONTROLLER_TOGGLE) {
            if (edges & bit) { toggleOn[i] = !toggleOn[i]; toggleMoved[i] = moving; }
            if (toggleOn[i] && moving) toggleMoved[i] = 1;
            if (toggleOn[i] && ((toggleMoved[i] && !moving) || !next.connected)) toggleOn[i] = 0;
        }
        if (kind == CONTROLLER_AIM && (physical & bit)) {
            uint64_t fire = 1ULL << controller_mode_fire(controllerModes[i]);
            suppressed |= fire;
            if (!(physical & fire)) synthetic |= controllerMap[i];
        }
        int active = kind == CONTROLLER_TAP ? now < tapUntil[i] : kind == CONTROLLER_TOGGLE ? toggleOn[i] : 0;
        if (active) synthetic |= controllerMap[i];
    }
    uint64_t held = physical & ~suppressed;
    for (int i = 0; i < CONTROLLER_BUTTONS; i++) if (controllerModes[i] != CONTROLLER_HOLD) held &= ~(1ULL << i);
    next.buttons = controller_map_apply(controllerMap, held) | synthetic;
    pressedEdges |= next.buttons & ~gamepadState.buttons;
    gamepadState = next;
}

void runtime_gamepad_start(void) {
    pthread_mutex_lock(&gamepadLock);
    if (!started) { started = 1; load_controller_map(); }
    pthread_mutex_unlock(&gamepadLock);
}

/* Focus lost or the input queue went away: Android sends no releases for what was held then. */
void runtime_gamepad_clear(void) {
    pthread_mutex_lock(&gamepadLock);
    keyButtons = 0; faceA = faceB = faceX = faceY = 0;
    axisLX = axisLY = axisRX = axisRY = axisLT = axisRT = hatX = hatY = 0;
    physicalPrev = 0;
    memset(tapUntil, 0, sizeof tapUntil); memset(toggleOn, 0, sizeof toggleOn); memset(toggleMoved, 0, sizeof toggleMoved);
    gamepadState = (RuntimeGamepadState){0}; gamepadState.connected = connected;
    pressedEdges = 0;
    pthread_mutex_unlock(&gamepadLock);
}

void runtime_gamepad_read(RuntimeGamepadState *out) {
    pthread_mutex_lock(&gamepadLock);
    if (started) refresh_locked();
    *out = gamepadState;
    out->buttons |= pressedEdges;
    pressedEdges = 0;
    pthread_mutex_unlock(&gamepadLock);
}

void runtime_rumble_set(unsigned side, float al, float fl, float ah, float fh) { (void)side; (void)al; (void)fl; (void)ah; (void)fh; }

#ifdef SCARLET_APK
/* Physical button of an Android key code (bit of controller_physical_names), -1 for the face buttons (kept by name),
   -2 for keys the game does not take (the system's: home, volume, recent apps). */
static int key_button(int32_t code) {
    switch (code) {
    case AKEYCODE_BUTTON_A: case AKEYCODE_BUTTON_B: case AKEYCODE_BUTTON_X: case AKEYCODE_BUTTON_Y: return -1;
    case AKEYCODE_BUTTON_THUMBL: return 4;
    case AKEYCODE_BUTTON_THUMBR: return 5;
    case AKEYCODE_BUTTON_L1: return 6;
    case AKEYCODE_BUTTON_R1: return 7;
    case AKEYCODE_BUTTON_L2: return 8;
    case AKEYCODE_BUTTON_R2: return 9;
    case AKEYCODE_BUTTON_START: return 10;
    case AKEYCODE_BUTTON_SELECT: return 11;
    case AKEYCODE_DPAD_LEFT: return 12;
    case AKEYCODE_DPAD_UP: return 13;
    case AKEYCODE_DPAD_RIGHT: return 14;
    case AKEYCODE_DPAD_DOWN: return 15;
    default: return -2;
    }
}

/* One input event of the window (the input thread of runtime/activity_android.c); 1 = the game took it. Back is taken
   too, so the pad's back button does not close the game; home, volume and recent apps stay with Android. */
int runtime_android_input(const AInputEvent *event) {
    int32_t type = AInputEvent_getType(event), source = AInputEvent_getSource(event);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t code = AKeyEvent_getKeyCode(event), action = AKeyEvent_getAction(event);
        if (code == AKEYCODE_BACK) return 1;
        int button = key_button(code);
        if (button == -2 || action == AKEY_EVENT_ACTION_MULTIPLE) return 0;
        int down = action == AKEY_EVENT_ACTION_DOWN;
        pthread_mutex_lock(&gamepadLock);
        if (!connected && (source & (AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_JOYSTICK))) connected = 1;
        if (byPosition < 0) byPosition = gamepad_face_by_position(NULL, NULL);
        switch (code) {
        case AKEYCODE_BUTTON_A: faceA = down; break;
        case AKEYCODE_BUTTON_B: faceB = down; break;
        case AKEYCODE_BUTTON_X: faceX = down; break;
        case AKEYCODE_BUTTON_Y: faceY = down; break;
        default: if (down) keyButtons |= 1ULL << button; else keyButtons &= ~(1ULL << button);
        }
        if (started) refresh_locked();
        pthread_mutex_unlock(&gamepadLock);
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION && (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        float lt = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0), brake = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_BRAKE, 0);
        float rt = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0), gas = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_GAS, 0);
        pthread_mutex_lock(&gamepadLock);
        connected = 1;
        if (byPosition < 0) byPosition = gamepad_face_by_position(NULL, NULL);
        axisLX = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0);
        axisLY = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0);
        axisRX = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0);
        axisRY = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0);
        axisLT = lt > brake ? lt : brake; axisRT = rt > gas ? rt : gas;
        hatX = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
        hatY = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
        if (started) refresh_locked();
        pthread_mutex_unlock(&gamepadLock);
        return 1;
    }
    return 0;
}
#endif
