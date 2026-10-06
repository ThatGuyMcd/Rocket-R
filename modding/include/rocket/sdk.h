#ifndef ROCKET_SDK_H
#define ROCKET_SDK_H
#include "mod.h"
#include "sdk_types.h"
ROCKET_IMPORT RocketSdkI32 rocket_actor_pose(const RocketActorPose *pose) {
  (void)pose;
  return ROCKET_UNAVAILABLE;
}
/* Buttons declared in metadata are shown in this mod's settings. The game
 * thread consumes presses here; UI callbacks never run guest code directly. */
ROCKET_IMPORT RocketSdkU32 rocket_command_take(const char *name) {
  (void)name;
  return 0;
}

/* Register in rocket_on_game_ready. These imports are additional to API 1. */
ROCKET_IMPORT RocketSdkU32 rocket_sdk_version(void) { return 0; }
ROCKET_IMPORT RocketSdkU32 rocket_sdk_module(const char *name) {
  (void)name;
  return 0;
}
ROCKET_IMPORT RocketSdkI32
rocket_sdk_register(const RocketCallbacks *callbacks) {
  (void)callbacks;
  return ROCKET_UNAVAILABLE;
}
ROCKET_IMPORT RocketSdkU32 rocket_sdk_enabled(void) { return 0; }
ROCKET_IMPORT RocketSdkI32 rocket_sdk_log(const char *message) {
  (void)message;
  return ROCKET_UNAVAILABLE;
}
ROCKET_IMPORT RocketSdkI32 rocket_input_action(const char *name,
                                               RocketInputState *output) {
  (void)name;
  (void)output;
  return ROCKET_UNAVAILABLE;
}
/* Resource names are scoped to the calling mod. Reads copy raw bytes into a
 * caller-owned buffer. A handle remains valid until close or deactivation. */
ROCKET_IMPORT RocketSdkU32 rocket_resource_open(const char *name) {
  (void)name;
  return 0;
}
ROCKET_IMPORT RocketSdkI32 rocket_resource_size(RocketSdkU32 handle) {
  (void)handle;
  return ROCKET_UNAVAILABLE;
}
ROCKET_IMPORT RocketSdkI32 rocket_resource_read(RocketSdkU32 handle,
                                                RocketSdkU32 offset,
                                                void *buffer,
                                                RocketSdkU32 capacity) {
  (void)handle;
  (void)offset;
  (void)buffer;
  (void)capacity;
  return ROCKET_UNAVAILABLE;
}
ROCKET_IMPORT RocketSdkI32 rocket_resource_close(RocketSdkU32 handle) {
  (void)handle;
  return ROCKET_UNAVAILABLE;
}
/* Per-mod/profile saves have an explicit schema version. Read reports the
 * stored schema through schema_out; the mod owns any migration decision. */
ROCKET_IMPORT RocketSdkI32 rocket_save_write(const char *name,
                                             RocketSdkU32 schema,
                                             const void *buffer,
                                             RocketSdkU32 length) {
  (void)name;
  (void)schema;
  (void)buffer;
  (void)length;
  return ROCKET_UNAVAILABLE;
}
ROCKET_IMPORT RocketSdkI32 rocket_save_read(const char *name, void *buffer,
                                            RocketSdkU32 capacity,
                                            RocketSdkU32 *schema_out) {
  (void)name;
  (void)buffer;
  (void)capacity;
  (void)schema_out;
  return ROCKET_UNAVAILABLE;
}
ROCKET_IMPORT RocketSdkU32 rocket_player_handle(void) { return 0; }
ROCKET_IMPORT RocketSdkU32 rocket_object_observe(RocketSdkU32 guest_address) {
  (void)guest_address;
  return 0;
}
ROCKET_IMPORT RocketSdkI32 rocket_object_read(RocketSdkU32 handle,
                                              RocketObjectState *output) {
  (void)handle;
  (void)output;
  return ROCKET_UNAVAILABLE;
}
/* Uses the native virtual setters, preserving their collision/spatial updates.
 * Mutations are only accepted inside an active gameplay callback. */
ROCKET_IMPORT RocketSdkI32 rocket_object_position(RocketSdkU32 handle,
                                                  const RocketVec3 *value) {
  (void)handle;
  (void)value;
  return ROCKET_UNAVAILABLE;
}
ROCKET_IMPORT RocketSdkI32 rocket_object_velocity(RocketSdkU32 handle,
                                                  const RocketVec3 *value) {
  (void)handle;
  (void)value;
  return ROCKET_UNAVAILABLE;
}

ROCKET_IMPORT RocketSdkI32 rocket_event_subscribe(const char *name,
                                                  RocketEventHandler handler) {
  (void)name;
  (void)handler;
  return ROCKET_UNAVAILABLE;
}
ROCKET_IMPORT RocketSdkI32 rocket_event_emit(const char *name, const void *data,
                                             RocketSdkU32 length) {
  (void)name;
  (void)data;
  (void)length;
  return ROCKET_UNAVAILABLE;
}
/* Declare systems in rocket.json before requesting ownership. Ownership is
 * arbitration for cooperating code, not permission to remove port fixes. */
ROCKET_IMPORT RocketSdkI32 rocket_system_claim(const char *name) {
  (void)name;
  return ROCKET_UNAVAILABLE;
}
ROCKET_IMPORT RocketSdkI32 rocket_system_release(const char *name) {
  (void)name;
  return ROCKET_UNAVAILABLE;
}
/* The last submitted list persists until replaced, scene exit or disable.
 * Submit zero items to clear it. Up to 64 items per mod, 256 in the session. */
ROCKET_IMPORT RocketSdkI32 rocket_hud_submit(const RocketHudItem *items,
                                             RocketSdkU32 count) {
  (void)items;
  (void)count;
  return ROCKET_UNAVAILABLE;
}
/* Native effects retain the game's mixer and timing. IDs 0..161, volume/pan
 * 0..128. Handle ownership is checked; sounds stop on scene exit/disable. */
ROCKET_IMPORT RocketSdkU32 rocket_sound_play(RocketSdkU32 effect,
                                             RocketSdkU32 volume,
                                             RocketSdkU32 pan) {
  (void)effect;
  (void)volume;
  (void)pan;
  return 0;
}
ROCKET_IMPORT RocketSdkI32 rocket_sound_stop(RocketSdkU32 handle) {
  (void)handle;
  return ROCKET_UNAVAILABLE;
}
ROCKET_IMPORT RocketSdkU32 rocket_audio_load(const char *resource) {
  (void)resource;
  return 0;
}
ROCKET_IMPORT RocketSdkU32 rocket_audio_play(const RocketAudioPlay *play) {
  (void)play;
  return 0;
}
ROCKET_IMPORT RocketSdkI32 rocket_audio_stop(RocketSdkU32 voice) {
  (void)voice;
  return ROCKET_UNAVAILABLE;
}
ROCKET_IMPORT RocketSdkU32 rocket_audio_playing(RocketSdkU32 voice) {
  (void)voice;
  return 0;
}

ROCKET_IMPORT RocketSdkU32 rocket_mesh_load(const char *resource) {
  (void)resource;
  return 0;
}
ROCKET_IMPORT RocketSdkU32 rocket_actor_create(const RocketActorState *state) {
  (void)state;
  return 0;
}
ROCKET_IMPORT RocketSdkI32 rocket_actor_read(RocketSdkU32 actor,
                                             RocketActorState *state) {
  (void)actor;
  (void)state;
  return ROCKET_UNAVAILABLE;
}
ROCKET_IMPORT RocketSdkI32 rocket_actor_update(RocketSdkU32 actor,
                                               const RocketActorState *state) {
  (void)actor;
  (void)state;
  return ROCKET_UNAVAILABLE;
}
ROCKET_IMPORT RocketSdkI32 rocket_actor_remove(RocketSdkU32 actor) {
  (void)actor;
  return ROCKET_UNAVAILABLE;
}
/* Custom scene files describe meshes, actors and box collision. This adds
 * content to the current game scene; it does not load a retail level. */
ROCKET_IMPORT RocketSdkI32 rocket_scene_load(const char *resource) {
  (void)resource;
  return ROCKET_UNAVAILABLE;
}
ROCKET_IMPORT RocketSdkI32 rocket_scene_clear(void) {
  return ROCKET_UNAVAILABLE;
}
ROCKET_IMPORT RocketSdkI32 rocket_scene_load_at(const char *resource,
                                                const RocketVec3 *origin) {
  (void)resource;
  (void)origin;
  return ROCKET_UNAVAILABLE;
}
ROCKET_IMPORT RocketSdkU32 rocket_actor_find(const char *name) {
  (void)name;
  return 0;
}
ROCKET_IMPORT RocketSdkI32 rocket_collision_move(RocketMotion *motion) {
  (void)motion;
  return ROCKET_UNAVAILABLE;
}
/* Claim world.render before hiding the native 3D render queue. Managed
 * models still render; scene exit/disable restores the native queue. */
ROCKET_IMPORT RocketSdkI32 rocket_native_render(RocketSdkU32 visible) {
  (void)visible;
  return ROCKET_UNAVAILABLE;
}
/* Claim audio.music to attenuate the native music while custom music plays.
 * Effects retain their volume; cleanup restores the captured music gain. */
ROCKET_IMPORT RocketSdkI32 rocket_native_music_gain(RocketSdkU32 percent) {
  (void)percent;
  return ROCKET_UNAVAILABLE;
}
/* Queries test this mod's custom collision, not the retail collision mesh. */
ROCKET_IMPORT RocketSdkI32 rocket_collision_ray(const RocketRay *ray,
                                                RocketHit *hit) {
  (void)ray;
  (void)hit;
  return ROCKET_UNAVAILABLE;
}

#endif
