#include "rocket/sdk.h"

static RocketSdkU32 count, loaded, tone;
static RocketSdkU32 marker;
static RocketSdkU32 room_active, stage, mask, gears, hero, hazard, music_clip,
    music_voice;
static RocketVec3 return_position, origin, previous;
static float patrol_direction = 1, hit_cooldown;
static char title[64] = "Rocket Workshop";
static RocketHudItem hud[2];

static unsigned append(char *out, unsigned pos, const char *text) {
  while (*text && pos < 158)
    out[pos++] = *text++;
  out[pos] = 0;
  return pos;
}
static unsigned decimal(char *out, unsigned pos, unsigned value) {
  char digits[10];
  unsigned length = 0;
  do {
    digits[length++] = (char)('0' + value % 10);
    value /= 10;
  } while (value && length < 10);
  while (length && pos < 158)
    out[pos++] = digits[--length];
  out[pos] = 0;
  return pos;
}
static void update_hud(void) {
  if (!recomp_get_config_u32("show_hud")) {
    rocket_hud_submit(0, 0);
    return;
  }
  hud[0].api = hud[1].api = ROCKET_SDK_VERSION;
  hud[0].size = hud[1].size = sizeof(RocketHudItem);
  hud[0].kind = ROCKET_HUD_RECTANGLE;
  hud[0].rgba = 0x06202BCC;
  hud[0].x = 12;
  hud[0].y = 12;
  hud[0].width = 230;
  hud[0].height = 46;
  hud[1].kind = ROCKET_HUD_TEXT;
  hud[1].rgba = 0xD9F7FFFF;
  hud[1].x = 18;
  hud[1].y = 16;
  hud[1].height = 9;
  unsigned pos = append(hud[1].text, 0, title);
  pos = append(hud[1].text, pos, "\nBoosts: ");
  pos = decimal(hud[1].text, pos, count);
  if (room_active) {
    pos = append(hud[1].text, pos, "   Gears: ");
    pos = decimal(hud[1].text, pos, gears);
    append(hud[1].text, pos,
           stage >= 2 ? "\nWorkshop complete!"
                      : "\nCollect the gold blocks. Avoid the red patrol.");
  } else
    append(hud[1].text, pos, "\nV / right shoulder / touch R");
  rocket_hud_submit(hud, 2);
}
static RocketSdkU32 decode(const unsigned char *data) {
  return ((RocketSdkU32)data[0] << 24) | ((RocketSdkU32)data[1] << 16) |
         ((RocketSdkU32)data[2] << 8) | data[3];
}
static void persist(void) {
  unsigned char bytes[16];
  RocketSdkU32 values[4] = {count, gears, stage, mask};
  for (unsigned i = 0; i < 4; ++i)
    for (unsigned j = 0; j < 4; ++j)
      bytes[i * 4 + j] = (unsigned char)(values[i] >> (24 - j * 8));
  rocket_save_write("progress", 2, bytes, sizeof(bytes));
}
static int player_read(const RocketTick *tick, RocketObjectState *player) {
  player->api = 2;
  player->size = sizeof(*player);
  return rocket_object_read(tick->player, player) == ROCKET_OK;
}
static float distance_squared(RocketVec3 a, RocketVec3 b) {
  const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
  return x * x + y * y + z * z;
}
static void update_music(void) {
  if (room_active && recomp_get_config_u32("music")) {
    if (!rocket_audio_playing(music_voice) &&
        rocket_system_claim("audio.music") == ROCKET_OK) {
      if (!music_clip)
        music_clip = rocket_audio_load("music");
      RocketAudioPlay play = {2, sizeof(play), music_clip, 1, .6F, 0};
      music_voice = rocket_audio_play(&play);
      if (music_voice)
        rocket_native_music_gain(0);
    }
  } else {
    if (music_voice)
      rocket_audio_stop(music_voice);
    music_voice = 0;
    if (room_active)
      rocket_native_music_gain(100);
  }
}
static int load_room(void) {
  if (rocket_scene_load_at(stage == 0 ? "room1" : "room2", &origin) !=
      ROCKET_OK) {
    rocket_sdk_log("Could not load the Workshop room.");
    return 0;
  }
  hazard = rocket_actor_find("hazard");
  hero = rocket_actor_find("hero");
  patrol_direction = 1;
  hit_cooldown = 0;
  for (unsigned i = 0; i < 3; ++i)
    if (mask & (1U << i)) {
      char name[6] = {'g', 'e', 'a', 'r', (char)('0' + i), 0};
      rocket_actor_remove(rocket_actor_find(name));
    }
  return hero != 0;
}
static void room_exit(const RocketTick *tick) {
  if (!room_active)
    return;
  RocketObjectState player = {0};
  if (player_read(tick, &player)) {
    RocketVec3 stopped = {0};
    rocket_object_position(tick->player, &return_position);
    rocket_object_velocity(tick->player, &stopped);
  }
  if (music_voice)
    rocket_audio_stop(music_voice);
  music_voice = 0;
  rocket_native_music_gain(100);
  rocket_native_render(1);
  rocket_system_release("world.render");
  rocket_system_release("audio.music");
  rocket_scene_clear();
  room_active = 0;
  marker = hero = hazard = 0;
}
static void apply_room_option(const RocketTick *tick) {
  RocketObjectState player = {0};
  if (!player_read(tick, &player))
    return;
  if (recomp_get_config_u32("arena") && !room_active) {
    if (rocket_system_claim("world.render") != ROCKET_OK) {
      rocket_sdk_log("Another mod controls the native world rendering.");
      return;
    }
    return_position = player.position;
    origin = return_position;
    // Keep the native camera and player clear of the retail level's geometry.
    // Room geometry remains in gameplay units; this is a placement offset.
    origin.z += 512;
    previous = origin;
    if (!load_room()) {
      rocket_system_release("world.render");
      return;
    }
    RocketVec3 stopped = {0};
    rocket_object_position(tick->player, &origin);
    rocket_object_velocity(tick->player, &stopped);
    rocket_native_render(0);
    room_active = 1;
  } else if (!recomp_get_config_u32("arena") && room_active)
    room_exit(tick);
  update_music();
  update_hud();
}
static void arena_update(const RocketTick *tick) {
  RocketObjectState player = {0};
  if (!player_read(tick, &player))
    return;
  RocketMotion move = {2,
                       sizeof(move),
                       previous,
                       {player.position.x - previous.x,
                        player.position.y - previous.y,
                        player.position.z - previous.z},
                       {1, 1, 1.5F},
                       {0},
                       0};
  if (rocket_collision_move(&move) == ROCKET_OK) {
    previous = move.position;
    player.position = move.position;
    rocket_object_position(tick->player, &move.position);
    if (move.grounded && player.velocity.z < 0) {
      player.velocity.z = 0;
      rocket_object_velocity(tick->player, &player.velocity);
    }
  }
  RocketActorState actor = {0};
  actor.api = 2;
  actor.size = sizeof(actor);
  if (rocket_actor_read(hero, &actor) == ROCKET_OK) {
    actor.position = player.position;
    rocket_actor_update(hero, &actor);
  }
  if (rocket_actor_read(hazard, &actor) == ROCKET_OK) {
    actor.position.x += patrol_direction * tick->delta_time * 4.6875F;
    if (actor.position.x > origin.x + 8.75F)
      patrol_direction = -1;
    else if (actor.position.x < origin.x - 8.75F)
      patrol_direction = 1;
    actor.yaw += tick->delta_time;
    rocket_actor_update(hazard, &actor);
    if (hit_cooldown <= 0 &&
        distance_squared(actor.position, player.position) < 2.5F * 2.5F) {
      previous = origin;
      player.position = origin;
      RocketVec3 stopped = {0};
      rocket_object_position(tick->player, &origin);
      rocket_object_velocity(tick->player, &stopped);
      hit_cooldown = 1;
      if (tone)
        rocket_sound_stop(tone);
      tone = rocket_sound_play(0, 40, 64);
    }
  }
  if (hit_cooldown > 0)
    hit_cooldown -= tick->delta_time;
  for (unsigned i = 0; i < 3; ++i)
    if (!(mask & (1U << i))) {
      char name[6] = {'g', 'e', 'a', 'r', (char)('0' + i), 0};
      RocketSdkU32 token = rocket_actor_find(name);
      if (rocket_actor_read(token, &actor) != ROCKET_OK)
        continue;
      actor.yaw += tick->delta_time * 2;
      rocket_actor_update(token, &actor);
      if (distance_squared(actor.position, player.position) < 2.25F * 2.25F) {
        mask |= 1U << i;
        if (gears < 6)
          ++gears;
        rocket_actor_remove(token);
        rocket_event_emit("rocket_workshop:gear", &gears, sizeof(gears));
        persist();
        update_hud();
      }
    }
  if (mask == 7 && stage < 2) {
    ++stage;
    if (stage < 2) {
      mask = 0;
      previous = origin;
      load_room();
      rocket_object_position(tick->player, &origin);
    }
    persist();
    update_hud();
  }
}
static void scene_enter(const RocketTick *tick) {
  (void)tick;
  if (!loaded) {
    RocketSdkU32 schema = 0;
    unsigned char save[16] = {0};
    RocketSdkI32 length = rocket_save_read("progress", save, 16, &schema);
    if (length == 4 && schema == 1)
      count = decode(save);
    else if (length == 16 && schema == 2) {
      count = decode(save);
      gears = decode(save + 4);
      stage = decode(save + 8);
      mask = decode(save + 12);
      if (stage > 2 || mask > 7 || gears > 6)
        gears = stage = mask = 0;
    }
    RocketSdkU32 resource = rocket_resource_open("title");
    if (resource) {
      RocketSdkI32 length =
          rocket_resource_read(resource, 0, title, sizeof(title) - 1);
      if (length >= 0) {
        title[length] = 0;
        if (length && title[length - 1] == '\n')
          title[length - 1] = 0;
      }
      rocket_resource_close(resource);
    }
    loaded = 1;
  }
  if (rocket_system_claim("rocket_workshop.boost") != ROCKET_OK)
    rocket_sdk_log("Boost is already owned by another mod.");
  RocketObjectState player = {0};
  player.api = ROCKET_SDK_VERSION;
  player.size = sizeof(player);
  if (rocket_object_read(tick->player, &player) == ROCKET_OK) {
    RocketActorState actor = {ROCKET_SDK_VERSION,
                              sizeof(RocketActorState),
                              rocket_mesh_load("cube"),
                              1,
                              {0},
                              {0},
                              {1, 1, 1},
                              0};
    actor.position = player.position;
    actor.position.x += 6.25F;
    actor.position.z += 3.75F;
    marker = rocket_actor_create(&actor);
    RocketActorPose pose = {2, sizeof(pose), marker, .15F, .1F, 0, 1};
    rocket_actor_pose(&pose);
  }
  apply_room_option(tick);
}
static void update(const RocketTick *tick) {
  if (tick->state != ROCKET_PLAYING || tick->delta_time <= 0)
    return;
  if (rocket_command_take("restart")) {
    gears = stage = mask = 0;
    persist();
    if (room_active) {
      previous = origin;
      load_room();
      rocket_object_position(tick->player, &origin);
    }
    update_hud();
  }
  if (room_active)
    arena_update(tick);
  if (!room_active && marker) {
    RocketActorState actor = {0};
    actor.api = ROCKET_SDK_VERSION;
    actor.size = sizeof(actor);
    if (rocket_actor_read(marker, &actor) == ROCKET_OK) {
      actor.yaw += tick->delta_time;
      if (actor.yaw > 6.2831853F)
        actor.yaw -= 6.2831853F;
      rocket_actor_update(marker, &actor);
    }
  }
  RocketInputState input = {ROCKET_SDK_VERSION, sizeof(RocketInputState), 0, 0,
                            0};
  if (rocket_input_action("boost", &input) != ROCKET_OK || !input.pressed)
    return;
  RocketObjectState player = {0};
  player.api = ROCKET_SDK_VERSION;
  player.size = sizeof(player);
  if (rocket_object_read(tick->player, &player) != ROCKET_OK)
    return;
  player.velocity.z = (float)recomp_get_config_double("boost") / 16;
  if (rocket_object_velocity(tick->player, &player.velocity) != ROCKET_OK)
    return;
  if (count < 999999)
    ++count;
  persist();
  rocket_event_emit("rocket_workshop:boost", &count, sizeof(count));
  if (tone)
    rocket_sound_stop(tone);
  tone = rocket_sound_play(0, 64, 64);
  update_hud();
}
static void changed(const RocketTick *tick) {
  if (rocket_sdk_enabled() &&
      (tick->state == ROCKET_PLAYING || tick->state == ROCKET_PAUSED))
    apply_room_option(tick);
}
static void disabled(const RocketTick *tick) {
  room_exit(tick);
  tone = marker = music_clip = music_voice = 0;
}
static void scene_leave(const RocketTick *tick) {
  (void)tick;
  room_active = marker = hero = hazard = music_clip = music_voice = 0;
}
static const RocketCallbacks callbacks = {ROCKET_SDK_VERSION,
                                          sizeof(RocketCallbacks),
                                          update,
                                          scene_enter,
                                          scene_leave,
                                          0,
                                          disabled,
                                          changed};
ROCKET_CALLBACK(rocket_on_game_ready)
void workshop_ready(void) {
  if (rocket_sdk_register(&callbacks) == ROCKET_OK)
    rocket_sdk_log("Rocket Workshop 0.2.2 is ready.");
}
