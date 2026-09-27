#ifndef ROCKET_MOD_H
#define ROCKET_MOD_H

/* Rocket-R API 1. Guest pointers and integers use the N64 O32 ABI. */
#define ROCKET_CALLBACK(event) __attribute__((used, retain, section(".recomp_callback.*:" #event)))
#define ROCKET_IMPORT __attribute__((noinline, weak, used, section(".recomp_import.*")))
typedef unsigned int RocketU32;

/* Valid only during rocket_on_camera_update. Z is up; angles are radians.
 * Input fields are read-only. Set apply and the three output fields to request
 * an orbit before Rocket checks obstacles and smooths the camera. The yaw
 * and pitch include the game's previous avoidance correction; use them as the
 * starting point for each step. When recenter is set, yaw instead points
 * behind Rocket and pitch comes from the current original zoom preset.
 * Third-person and camera-volume views are supported. */
typedef struct RocketCamera {
    RocketU32 api, size;
    float delta_time, look_x, look_y;
    RocketU32 recenter, reset;
    float yaw, pitch, distance;
    RocketU32 apply;
    float output_yaw, output_pitch, output_distance;
} RocketCamera;

/* Optional rocket_on_mouse_look event, delivered immediately before the camera
 * update. Deltas are radians since the last update, independent of frame rate.
 * Positive yaw looks right; positive pitch moves the mouse down. */
typedef struct RocketMouseLook {
    RocketU32 api, size;
    float yaw, pitch;
} RocketMouseLook;

/* Optional rocket_on_first_person_update event. Angles are radians in the
 * same rotation direction as RocketCamera; positive pitch looks up. Recenter
 * supplies Rocket's facing direction and the original first-person entry pitch.
 * Set apply and the output angles. Rocket keeps its original view transition,
 * pose and pitch limits. This packet has its own ABI and does not alter RocketCamera. */
typedef struct RocketFirstPersonCamera {
    RocketU32 api, size;
    float delta_time, look_x, look_y;
    RocketU32 recenter, reset;
    float yaw, pitch;
    RocketU32 apply;
    float output_yaw, output_pitch;
} RocketFirstPersonCamera;

ROCKET_IMPORT void rocket_claim_analogue_camera(void) {}
ROCKET_IMPORT void rocket_enable_mouse_look(void) {}
ROCKET_IMPORT void rocket_enable_first_person_look(void) {}
/* Original camera follow smoothing: 0 is direct, 100 keeps the original feel.
 * Only applies while this mod owns the camera. Obstacle checks still run. */
ROCKET_IMPORT void rocket_set_camera_smoothing(RocketU32 percent) { (void)percent; }
ROCKET_IMPORT double recomp_get_config_double(const char* key) { (void)key; return 0; }
ROCKET_IMPORT RocketU32 recomp_get_config_u32(const char* key) { (void)key; return 0; }

#endif
