#include <rocket/mod.h>

static float yaw, pitch, horizontal_speed, vertical_speed;
static float sensitivity, deadzone, response, momentum;
static int initialized, invert_x, invert_y;
static float mouse_yaw, mouse_pitch;
static float first_person_horizontal_speed, first_person_vertical_speed;
static float mouse_horizontal_speed, mouse_vertical_speed;

static float clamp(float value, float low, float high) {
    return value < low ? low : value > high ? high : value;
}
static float shape(float axis) {
    float magnitude = axis < 0 ? -axis : axis;
    if (magnitude <= deadzone) return 0;
    float value = (magnitude - deadzone) / (1 - deadzone);
    return axis < 0 ? -value : value;
}

static float smooth(float target, float* speed, float dt, int mouse) {
    /* Reversing input must not fight momentum from the old direction. */
    if (target * *speed < 0) *speed = 0;
    float blend = mouse || target == 0
        ? (momentum == 0 ? 1 : dt / (0.25f * momentum + dt))
        : clamp(dt * response, 0, 1);
    *speed += (target - *speed) * blend;
    if (*speed < 0.0001f && *speed > -0.0001f) *speed = 0;
    return *speed * dt;
}
static void clear_speed(void) {
    horizontal_speed = vertical_speed = 0;
    first_person_horizontal_speed = first_person_vertical_speed = 0;
    mouse_horizontal_speed = mouse_vertical_speed = 0;
}
static float mouse_step(float delta, float* speed, float dt) {
    /* The retail solver chooses the shorter arc. A mouse burst must never
     * request half a revolution in one 30 Hz update and reverse that choice.
     * Bound speed before smoothing; discard excess rather than queue a spin. */
    return smooth(clamp(delta / dt, -8, 8), speed, dt, 1);
}

static void read_settings(void) {
    float next_sensitivity = (float)recomp_get_config_double("sensitivity");
    float next_deadzone = (float)recomp_get_config_double("deadzone");
    float next_response = (float)recomp_get_config_double("response");
    float next_momentum = clamp((float)recomp_get_config_double("momentum") / 100, 0, 1);
    int next_invert_y = recomp_get_config_u32("invert_y") != 0;
    int next_invert_x = recomp_get_config_u32("invert_x") != 0;
    if (sensitivity != next_sensitivity || deadzone != next_deadzone || response != next_response || momentum != next_momentum ||
        invert_y != next_invert_y || invert_x != next_invert_x) {
        /* Do not carry movement in the old direction across an inversion or
         * speed change. Keep the current view and collision-aware heading. */
        clear_speed();
    }
    sensitivity = next_sensitivity; deadzone = next_deadzone; response = next_response;
    momentum = next_momentum;
    rocket_set_camera_smoothing((RocketU32)(momentum * 100 + 0.5f));
    invert_y = next_invert_y; invert_x = next_invert_x;
}

ROCKET_CALLBACK(rocket_on_game_ready)
void camera_start(void) {
    read_settings();
    initialized = 0;
    mouse_yaw = mouse_pitch = 0;
    clear_speed();
    rocket_claim_analogue_camera();
    rocket_enable_mouse_look();
    rocket_enable_first_person_look();
}

ROCKET_CALLBACK(rocket_on_mouse_look)
void camera_mouse(RocketMouseLook* mouse) {
    if (mouse->api != 1 || mouse->size != sizeof(*mouse)) return;
    mouse_yaw += mouse->yaw;
    mouse_pitch += mouse->pitch;
}

ROCKET_CALLBACK(rocket_on_camera_update)
void camera_update(RocketCamera* camera) {
    if (camera->api != 1 || camera->size != sizeof(*camera)) return;
    read_settings();
    if (!initialized || camera->reset || camera->recenter) {
        yaw = camera->yaw;
        pitch = camera->pitch;
        clear_speed();
        initialized = 1;
    }
    /* The game may choose another heading to avoid an obstacle. Start each
     * horizontal step there instead of insisting on yesterday's blocked angle. */
    yaw = camera->yaw;
    /* Continue from the checked elevation too. Do not keep pushing a rejected
     * downward angle (or its momentum) after the floor has raised the camera. */
    float pitch_correction = camera->pitch - pitch;
    if (pitch_correction > 0.001f || pitch_correction < -0.001f)
        vertical_speed = mouse_vertical_speed = 0;
    pitch = camera->pitch;
    if (camera->recenter) {
        /* The host supplies the heading behind Rocket and the original zoom
         * pitch. Do not let a held stick or a mouse click's motion undo it. */
        mouse_yaw = mouse_pitch = 0;
        camera->output_yaw = yaw;
        camera->output_pitch = pitch;
        camera->output_distance = camera->distance;
        camera->apply = 1;
        return;
    }
    float dt = clamp(camera->delta_time, 0.001f, 0.1f);
    float dx = smooth(shape(camera->look_x) * sensitivity, &horizontal_speed, dt, 0) +
        mouse_step(mouse_yaw, &mouse_horizontal_speed, dt);
    float dy = smooth(shape(camera->look_y) * sensitivity, &vertical_speed, dt, 0) +
        mouse_step(mouse_pitch, &mouse_vertical_speed, dt);
    yaw += clamp(dx, -8 * dt, 8 * dt) * (invert_x ? 1 : -1);
    if (yaw > 3.14159265f) yaw -= 6.2831853f;
    if (yaw < -3.14159265f) yaw += 6.2831853f;
    pitch = clamp(pitch + clamp(dy, -8 * dt, 8 * dt) * (invert_y ? -1 : 1), -0.30f, 1.10f);
    mouse_yaw = mouse_pitch = 0;
    camera->output_yaw = yaw;
    camera->output_pitch = pitch;
    camera->output_distance = camera->distance;
    camera->apply = 1;
}

ROCKET_CALLBACK(rocket_on_first_person_update)
void camera_first_person(RocketFirstPersonCamera* camera) {
    if (camera->api != 1 || camera->size != sizeof(*camera)) return;
    read_settings();
    if (camera->reset || camera->recenter)
        clear_speed();
    if (camera->recenter) {
        mouse_yaw = mouse_pitch = 0;
        camera->output_yaw = camera->yaw;
        camera->output_pitch = camera->pitch;
        camera->apply = 1;
        return;
    }
    float dt = clamp(camera->delta_time, 0.001f, 0.1f);
    float dx = smooth(shape(camera->look_x) * sensitivity, &first_person_horizontal_speed, dt, 0) +
        mouse_step(mouse_yaw, &mouse_horizontal_speed, dt);
    float dy = smooth(shape(camera->look_y) * sensitivity, &first_person_vertical_speed, dt, 0) +
        mouse_step(mouse_pitch, &mouse_vertical_speed, dt);
    camera->output_yaw = camera->yaw + clamp(dx, -8 * dt, 8 * dt) * (invert_x ? 1 : -1);
    camera->output_pitch = clamp(camera->pitch + clamp(dy, -8 * dt, 8 * dt) * (invert_y ? -1 : 1), -1.0f, 1.0f);
    mouse_yaw = mouse_pitch = 0;
    camera->apply = 1;
}
