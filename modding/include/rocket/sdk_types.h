#ifndef ROCKET_SDK_TYPES_H
#define ROCKET_SDK_TYPES_H

/* Additive SDK 2 ABI. API 1 packets and imports remain in rocket/mod.h. */
#define ROCKET_SDK_VERSION 2U
typedef unsigned int RocketSdkU32;
typedef int RocketSdkI32;
typedef struct RocketVec3 {
  float x, y, z;
} RocketVec3;
enum RocketResult {
  ROCKET_OK = 0,
  ROCKET_INVALID = -1,
  ROCKET_NOT_FOUND = -2,
  ROCKET_UNAVAILABLE = -3,
  ROCKET_LIMIT = -4,
  ROCKET_CONFLICT = -5,
  ROCKET_IO_ERROR = -6,
  ROCKET_BUFFER_SMALL = -7
};
enum RocketGameState {
  ROCKET_FRONTEND = 0,
  ROCKET_LOADING = 1,
  ROCKET_PLAYING = 2,
  ROCKET_PAUSED = 3
};
/* All pointers below are guest addresses. Callbacks run on the game thread.
 * Packets expire on return. Do not retain packet pointers. */
typedef struct RocketTick {
  RocketSdkU32 api, size, state, scene, tick;
  float delta_time;
  RocketSdkU32 player;
} RocketTick;
typedef struct RocketCallbacks {
  RocketSdkU32 api, size;
  void (*tick)(const RocketTick *);
  void (*scene_enter)(const RocketTick *);
  void (*scene_leave)(const RocketTick *);
  void (*enabled)(const RocketTick *);
  void (*disabled)(const RocketTick *);
  void (*settings_changed)(const RocketTick *);
} RocketCallbacks;
/* Native observation handles expire at the next authored frame boundary.
 * The transform fields follow Rocket's Z-up coordinate system. */
typedef struct RocketObjectState {
  RocketSdkU32 api, size, handle, scene, guest_address;
  RocketVec3 position, velocity;
  float rotation[9];
} RocketObjectState;
typedef struct RocketInputState {
  RocketSdkU32 api, size;
  float value;
  /* Edges since this mod last read the action; not frame-rate dependent. */
  RocketSdkU32 pressed, released;
} RocketInputState;

/* Named mod events are queued until the next game-thread boundary. Names use
 * mod_id:event. Only that mod can emit the name; subscribers may read it. */
typedef struct RocketEvent {
  RocketSdkU32 api, size, tick, length;
  char name[96];
  unsigned char data[256];
} RocketEvent;
typedef void (*RocketEventHandler)(const RocketEvent *);

/* HUD coordinates are relative to a 320x240 canvas covering the viewport.
 * rgba is 0xRRGGBBAA. Text is UTF-8 and uses the game's UI font. */
enum RocketHudKind { ROCKET_HUD_TEXT = 0, ROCKET_HUD_RECTANGLE = 1 };
typedef struct RocketHudItem {
  RocketSdkU32 api, size, kind, rgba;
  float x, y, width, height;
  char text[160];
} RocketHudItem;

/* Managed actors have persistent handles until removal, scene exit or disable.
 * Meshes contain authored geometry and optional small RGBA16 textures. Native
 * objects use separate one-frame handles and are never created by these
 * functions. */
typedef struct RocketActorState {
  RocketSdkU32 api, size, mesh, visible;
  RocketVec3 position, velocity, scale;
  float yaw;
} RocketActorState;
typedef struct RocketRay {
  RocketSdkU32 api, size;
  RocketVec3 start, end;
} RocketRay;
typedef struct RocketHit {
  RocketSdkU32 api, size, collider;
  float fraction;
  RocketVec3 position, normal;
} RocketHit;

/* Custom WAV clips run through the existing audio output. Volume is 0..1,
 * pan is -1 (left)..1 (right). Voices pause outside active gameplay. */
typedef struct RocketAudioPlay {
  RocketSdkU32 api, size, clip, loop;
  float volume, pan;
} RocketAudioPlay;

/* Z-up axis-aligned character sweep against this mod's collision boxes.
 * position/displacement are updated in place; normal and grounded are outputs.
 */
typedef struct RocketMotion {
  RocketSdkU32 api, size;
  RocketVec3 position, displacement, half_extent, normal;
  RocketSdkU32 grounded;
} RocketMotion;

/* Optional full 3D orientation, composed with the actor's Z-up yaw.
 * Quaternion order: x, y, z, w. The host normalises finite, nonzero values. */
typedef struct RocketActorPose {
  RocketSdkU32 api, size, actor;
  float x, y, z, w;
} RocketActorPose;

#endif
