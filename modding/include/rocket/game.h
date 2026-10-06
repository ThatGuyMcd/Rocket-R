#ifndef ROCKET_GAME_H
#define ROCKET_GAME_H
#include "sdk_types.h"
/* Verified prefix of the US cartridge's GameObject. Raw pointers are original
 * game memory, not SDK handles. Prefer rocket_object_read for observations. */
typedef struct RocketNativeObject {
  RocketSdkU32 vtable, unknown4, unknown8, unknownC, parent, unknown14;
  float rotation[9];
  RocketVec3 position;
  unsigned char reserved48[0x30];
  RocketVec3 velocity;
} RocketNativeObject;
#if defined(__mips__)
_Static_assert(__builtin_offsetof(RocketNativeObject, position) == 0x3C,
               "GameObject position ABI");
_Static_assert(__builtin_offsetof(RocketNativeObject, velocity) == 0x78,
               "GameObject velocity ABI");
#endif
extern void obj_setter_position(RocketNativeObject *object,
                                const RocketVec3 *position);
extern void obj_setter_velocity(RocketNativeObject *object,
                                const RocketVec3 *velocity);
extern void obj_set_position_xyz(RocketNativeObject *object, float x, float y,
                                 float z);
extern RocketNativeObject *D_800AAF5C;
#endif
