#include "sdk_runtime.hpp"
#include "librecomp/addresses.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/overlays.hpp"
#include "rocket/sdk_types.h"
#include "runtime_input.hpp"
#include "sdk_audio.hpp"
#include "sdk_render.hpp"
#include "sdk_services.hpp"
#include "sdk_world.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <set>

namespace rocket::mods::sdk {
namespace {
Services services;
World world;
struct Client {
  bool registered = false, enabled = false, requested = true, notified = false,
       dirty = false, managed = false, api2 = false;
  std::array<std::uint32_t, 6> callbacks{};
  std::uint64_t ticks = 0;
  std::map<std::string, std::pair<std::uint32_t, std::uint32_t>> input_edges;
  std::map<std::string, std::uint32_t> subscriptions;
  std::set<std::string> systems;
  std::set<std::uint32_t> sounds;
  std::vector<RocketHudItem> hud;
  bool native_visible = true;
  int music_original = -1, music_last = -1;
  std::map<std::string, std::uint32_t> commands;
};
std::map<std::string, Client> clients;
std::vector<std::string> order;
std::map<std::string, std::string> system_owners;
std::deque<RocketEvent> events;
std::recursive_mutex mutex;
std::uint32_t scratch = 0, scene = 0, ticks = 0, scene_root = 0;
float previous_clock = -1;
bool in_gameplay = false, in_callback = false;
const Client *callback_client = nullptr;
bool disabling_callback = false;
RocketTick current{};
struct Object {
  std::uint32_t address, scene, type, parent;
};
std::map<std::uint32_t, Object> objects;
std::uint32_t next_object = 1;
gpr guest(std::uint32_t address) {
  return static_cast<gpr>(static_cast<std::int32_t>(address));
}
bool range(std::uint32_t address, std::uint32_t length, bool aligned = false) {
  return address >= 0x80000400U && address < 0xA0000000U &&
         length <= 0xA0000000U - address && (!aligned || (address & 3U) == 0);
}
bool original_range(std::uint32_t address, std::uint32_t length) {
  return address >= 0x80000400U && address < 0x80800000U &&
         length <= 0x80800000U - address && (address & 3U) == 0;
}
std::uint32_t word(std::uint8_t *rdram, std::uint32_t address) {
  std::uint32_t value;
  std::memcpy(&value, rdram + (address & 0x3FFFFFFFU), 4);
  return value;
}
void put(std::uint8_t *rdram, std::uint32_t address, std::uint32_t value) {
  std::memcpy(rdram + (address & 0x3FFFFFFFU), &value, 4);
}
float number(std::uint8_t *rdram, std::uint32_t address) {
  return std::bit_cast<float>(word(rdram, address));
}
std::string text(std::uint8_t *rdram, std::uint32_t address,
                 std::size_t limit) {
  if (!range(address, static_cast<std::uint32_t>(limit)))
    throw std::runtime_error("Invalid guest string.");
  std::string value;
  for (std::size_t i = 0; i < limit; ++i) {
    const auto byte =
        static_cast<char>(rdram[((address & 0x3FFFFFFFU) + i) ^ 3U]);
    if (byte == 0)
      return value;
    value.push_back(byte);
  }
  throw std::runtime_error("Guest string is too long.");
}
std::vector<std::uint8_t> bytes(std::uint8_t *rdram, std::uint32_t address,
                                std::uint32_t length) {
  if (length > 256 * 1024 || !range(address, length))
    throw std::runtime_error("Invalid guest byte buffer.");
  std::vector<std::uint8_t> value(length);
  for (std::uint32_t i = 0; i < length; ++i)
    value[i] = rdram[((address & 0x3FFFFFFFU) + i) ^ 3U];
  return value;
}
void copy_bytes(std::uint8_t *rdram, std::uint32_t address,
                std::span<const std::uint8_t> source) {
  if (!range(address, static_cast<std::uint32_t>(source.size())))
    throw std::runtime_error("Invalid guest output buffer.");
  for (std::size_t i = 0; i < source.size(); ++i)
    rdram[((address & 0x3FFFFFFFU) + i) ^ 3U] = source[i];
}
std::string owner(std::size_t index) {
  auto id = recomp::mods::get_mod_id(index);
  if (!clients.contains(id))
    throw std::runtime_error("Calling mod is not in this session.");
  return id;
}
template <class F> void guarded(recomp_context *ctx, F &&operation) {
  std::lock_guard lock(mutex);
  try {
    ctx->r2 = static_cast<gpr>(operation());
  } catch (const std::exception &e) {
    std::fprintf(stderr, "[mod-sdk] %s\n", e.what());
    ctx->r2 = ROCKET_INVALID;
  }
}
void call_packet(std::uint8_t *rdram, const recomp_context &source,
                 std::uint32_t callback, const void *packet, std::size_t size,
                 const Client &client, bool disabling = false) {
  if (callback == 0 || scratch == 0)
    return;
  for (std::size_t i = 0; i < size / 4; ++i) {
    std::uint32_t value;
    std::memcpy(&value, reinterpret_cast<const std::uint8_t *>(packet) + i * 4,
                4);
    put(rdram, scratch + 0xFC00 + static_cast<std::uint32_t>(i * 4), value);
  }
  auto ctx = source;
  ctx.r29 = guest(scratch + 0xFB00);
  ctx.r4 = guest(scratch + 0xFC00);
  const auto was_callback = in_callback;
  const auto previous_client = callback_client;
  const auto previous_disabling = disabling_callback;
  in_callback = true;
  callback_client = &client;
  disabling_callback = disabling;
  struct Restore {
    bool previous;
    const Client *client;
    bool disabling;
    ~Restore() {
      in_callback = previous;
      callback_client = client;
      disabling_callback = disabling;
    }
  } restore{was_callback, previous_client, previous_disabling};
  get_function(static_cast<std::int32_t>(callback))(rdram, &ctx);
}
void call(std::uint8_t *rdram, const recomp_context &source,
          std::uint32_t callback, const Client &client,
          bool disabling = false) {
  call_packet(rdram, source, callback, &current, sizeof(current), client,
              disabling);
}
bool callback_address(std::uint32_t address) {
  return address >= 0x81000000U && range(address, 4, true);
}
bool event_name(const std::string &name) {
  const auto colon = name.find(':');
  return colon != std::string::npos && colon > 0 && name.size() < 96 &&
         valid_id(name.substr(0, colon)) && valid_id(name.substr(colon + 1));
}
void cleanup(std::uint8_t *rdram, const recomp_context &source,
             const std::string &id) {
  auto &client = clients.at(id);
  rocket::input::set_mod_actions_enabled(id, false);
  for (auto &[name, edges] : client.input_edges) {
    const auto value = rocket::input::mod_action_state(id, name);
    edges = {value.presses, value.releases};
  }
  client.hud.clear();
  client.native_visible = true;
  for (auto &[name, count] : client.commands)
    count = 0;
  if (client.music_original >= 0) {
    std::uint16_t current_volume;
    std::memcpy(&current_volume, rdram + ((0x800BD2EA & 0x3FFFFFFF) ^ 2U), 2);
    if (current_volume == client.music_last) {
      auto ctx = source;
      ctx.r29 = guest(scratch + 0xFB00);
      ctx.r4 = 2;
      ctx.r5 = client.music_original;
      get_function(static_cast<std::int32_t>(0x8000546CU))(rdram, &ctx);
    }
    client.music_original = client.music_last = -1;
  }
  world.clear(id);
  audio().clear(id);
  for (auto handle : client.sounds) {
    auto ctx = source;
    ctx.r29 = guest(scratch + 0xFB00);
    ctx.r4 = handle;
    ctx.r5 = 0;
    get_function(static_cast<std::int32_t>(0x80005A08U))(rdram, &ctx);
  }
  client.sounds.clear();
  for (auto it = system_owners.begin(); it != system_owners.end();)
    if (it->second == id)
      it = system_owners.erase(it);
    else
      ++it;
}
std::uint32_t observe(std::uint8_t *rdram, std::uint32_t address) {
  if (!in_gameplay || !original_range(address, 0x184))
    return 0;
  const auto type = word(rdram, address), parent = word(rdram, address + 0x10);
  if (!original_range(type, 0x70))
    return 0;
  for (const auto &[handle, object] : objects)
    if (object.address == address && object.scene == scene &&
        object.type == type && object.parent == parent)
      return handle;
  if (objects.size() >= 4096 || next_object == 0)
    return 0;
  const auto handle = next_object++;
  objects.emplace(handle, Object{address, scene, type, parent});
  return handle;
}
const Object *object(std::uint8_t *rdram, std::uint32_t handle) {
  const auto found = objects.find(handle);
  if (!in_gameplay || found == objects.end())
    return nullptr;
  const auto &value = found->second;
  if (value.scene != scene || word(rdram, value.address) != value.type ||
      word(rdram, value.address + 0x10) != value.parent)
    return nullptr;
  return &value;
}
void version(std::uint8_t *, recomp_context *ctx) {
  ctx->r2 = ROCKET_SDK_VERSION;
}
void module(std::uint8_t *rdram, recomp_context *ctx) {
  guarded(ctx, [&] {
    return module_version(text(rdram, static_cast<std::uint32_t>(ctx->r4), 97));
  });
}
void register_client(std::uint8_t *rdram, recomp_context *ctx,
                     std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    auto &client = clients.at(id);
    const auto address = static_cast<std::uint32_t>(ctx->r4);
    constexpr unsigned callback_bytes =
        8 + 6 * 4; // Guest O32 pointers, including on a 64-bit host.
    if (!client.api2 || !range(address, callback_bytes, true) ||
        word(rdram, address) != 2 ||
        word(rdram, address + 4) < callback_bytes || client.registered)
      return ROCKET_INVALID;
    std::array<std::uint32_t, 6> callbacks{};
    for (unsigned i = 0; i < callbacks.size(); ++i) {
      callbacks[i] = word(rdram, address + 8 + i * 4);
      if (callbacks[i] != 0 &&
          (!range(callbacks[i], 4, true) || callbacks[i] < 0x81000000U))
        return ROCKET_INVALID;
    }
    client.callbacks = callbacks;
    client.registered = true;
    return ROCKET_OK;
  });
}
void enabled(std::uint8_t *, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] { return clients.at(owner(index)).enabled ? 1 : 0; });
}
void log(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    std::fprintf(
        stderr, "[mod:%s] %s\n", id.c_str(),
        text(rdram, static_cast<std::uint32_t>(ctx->r4), 1024).c_str());
    return ROCKET_OK;
  });
}
void input_action(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index),
               name = text(rdram, static_cast<std::uint32_t>(ctx->r4), 97);
    const auto output = static_cast<std::uint32_t>(ctx->r5);
    auto &edges = clients.at(id).input_edges;
    const auto found = edges.find(name);
    if (found == edges.end())
      return ROCKET_NOT_FOUND;
    if (!range(output, sizeof(RocketInputState), true) ||
        word(rdram, output) != 2 ||
        word(rdram, output + 4) < sizeof(RocketInputState))
      return ROCKET_INVALID;
    const auto value = rocket::input::mod_action_state(id, name);
    auto &previous = found->second;
    put(rdram, output + 8, std::bit_cast<std::uint32_t>(value.value));
    put(rdram, output + 12, value.presses - previous.first);
    put(rdram, output + 16, value.releases - previous.second);
    previous = {value.presses, value.releases};
    return ROCKET_OK;
  });
}
void resource_open(std::uint8_t *rdram, recomp_context *ctx,
                   std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    if (!clients.at(id).requested)
      return 0U;
    return services.open(id,
                         text(rdram, static_cast<std::uint32_t>(ctx->r4), 241));
  });
  if (ctx->r2 < 0)
    ctx->r2 = 0;
}
void resource_size(std::uint8_t *, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    return services.size(owner(index), static_cast<std::uint32_t>(ctx->r4));
  });
}
void resource_read(std::uint8_t *rdram, recomp_context *ctx,
                   std::size_t index) {
  guarded(ctx, [&]() -> int {
    if (static_cast<std::uint32_t>(ctx->r7) > 256 * 1024)
      return ROCKET_LIMIT;
    auto data = services.read(owner(index), static_cast<std::uint32_t>(ctx->r4),
                              static_cast<std::uint32_t>(ctx->r5),
                              static_cast<std::uint32_t>(ctx->r7));
    copy_bytes(rdram, static_cast<std::uint32_t>(ctx->r6), data);
    return static_cast<int>(data.size());
  });
}
void resource_close(std::uint8_t *, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    return services.close(owner(index), static_cast<std::uint32_t>(ctx->r4))
               ? ROCKET_OK
               : ROCKET_NOT_FOUND;
  });
}
void save_write(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    auto data = bytes(rdram, static_cast<std::uint32_t>(ctx->r6),
                      static_cast<std::uint32_t>(ctx->r7));
    services.save(id, text(rdram, static_cast<std::uint32_t>(ctx->r4), 97),
                  static_cast<std::uint32_t>(ctx->r5), data);
    return ROCKET_OK;
  });
}
void save_read(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&]() -> int {
    auto data = services.load(
        owner(index), text(rdram, static_cast<std::uint32_t>(ctx->r4), 97));
    if (data.second.size() > static_cast<std::uint32_t>(ctx->r6))
      return ROCKET_BUFFER_SMALL;
    if (!range(static_cast<std::uint32_t>(ctx->r7), 4, true))
      return ROCKET_INVALID;
    copy_bytes(rdram, static_cast<std::uint32_t>(ctx->r5), data.second);
    put(rdram, static_cast<std::uint32_t>(ctx->r7), data.first);
    return static_cast<int>(data.second.size());
  });
}
void player(std::uint8_t *rdram, recomp_context *ctx) {
  std::lock_guard lock(mutex);
  ctx->r2 = observe(rdram, word(rdram, 0x800AAF5C));
}
void object_observe(std::uint8_t *rdram, recomp_context *ctx) {
  std::lock_guard lock(mutex);
  ctx->r2 = observe(rdram, static_cast<std::uint32_t>(ctx->r4));
}
void object_read(std::uint8_t *rdram, recomp_context *ctx) {
  guarded(ctx, [&] {
    const auto *value = object(rdram, static_cast<std::uint32_t>(ctx->r4));
    const auto output = static_cast<std::uint32_t>(ctx->r5);
    if (!value)
      return ROCKET_NOT_FOUND;
    if (!range(output, sizeof(RocketObjectState), true) ||
        word(rdram, output) != 2 ||
        word(rdram, output + 4) < sizeof(RocketObjectState))
      return ROCKET_INVALID;
    put(rdram, output + 8, static_cast<std::uint32_t>(ctx->r4));
    put(rdram, output + 12, scene);
    put(rdram, output + 16, value->address);
    for (unsigned i = 0; i < 3; ++i) {
      put(rdram, output + 20 + i * 4,
          word(rdram, value->address + 0x3C + i * 4));
      put(rdram, output + 32 + i * 4,
          word(rdram, value->address + 0x78 + i * 4));
    }
    for (unsigned i = 0; i < 9; ++i)
      put(rdram, output + 44 + i * 4,
          word(rdram, value->address + 0x18 + i * 4));
    return ROCKET_OK;
  });
}
void mutate(std::uint8_t *rdram, recomp_context *ctx, unsigned setter) {
  guarded(ctx, [&] {
    const auto *value = object(rdram, static_cast<std::uint32_t>(ctx->r4));
    const auto vector = static_cast<std::uint32_t>(ctx->r5);
    if (!in_callback || !value || !callback_client ||
        (!callback_client->enabled && !disabling_callback))
      return ROCKET_UNAVAILABLE;
    if (!range(vector, 12, true))
      return ROCKET_INVALID;
    for (unsigned i = 0; i < 3; ++i)
      if (!std::isfinite(number(rdram, vector + i * 4)) ||
          std::fabs(number(rdram, vector + i * 4)) > 1000000)
        return ROCKET_INVALID;
    const auto function = word(rdram, value->type + setter);
    if (!original_range(function, 4))
      return ROCKET_UNAVAILABLE;
    auto call_context = *ctx;
    call_context.r4 = guest(value->address);
    call_context.r5 = guest(vector);
    get_function(static_cast<std::int32_t>(function))(rdram, &call_context);
    return ROCKET_OK;
  });
}
void position(std::uint8_t *rdram, recomp_context *ctx) {
  mutate(rdram, ctx, 0x58);
}
void velocity(std::uint8_t *rdram, recomp_context *ctx) {
  mutate(rdram, ctx, 0x60);
}
void event_subscribe(std::uint8_t *rdram, recomp_context *ctx,
                     std::size_t index) {
  guarded(ctx, [&] {
    auto &client = clients.at(owner(index));
    const auto name = text(rdram, static_cast<std::uint32_t>(ctx->r4), 96);
    const auto handler = static_cast<std::uint32_t>(ctx->r5);
    if (!client.api2 || !event_name(name) ||
        (handler != 0 && !callback_address(handler)))
      return ROCKET_INVALID;
    if (handler == 0) {
      client.subscriptions.erase(name);
      return ROCKET_OK;
    }
    if (!client.subscriptions.contains(name) &&
        client.subscriptions.size() >= 128)
      return ROCKET_LIMIT;
    client.subscriptions[name] = handler;
    return ROCKET_OK;
  });
}
void event_emit(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    const auto name = text(rdram, static_cast<std::uint32_t>(ctx->r4), 96);
    if (!in_callback || !in_gameplay || !clients.at(id).enabled)
      return ROCKET_UNAVAILABLE;
    if (!event_name(name) || !name.starts_with(id + ":"))
      return ROCKET_INVALID;
    const auto length = static_cast<std::uint32_t>(ctx->r6);
    if (length > 256 || events.size() >= 128)
      return ROCKET_LIMIT;
    const auto payload =
        bytes(rdram, static_cast<std::uint32_t>(ctx->r5), length);
    RocketEvent event{};
    event.api = 2;
    event.size = sizeof(event);
    event.tick = ticks;
    event.length = length;
    std::memcpy(event.name, name.c_str(), name.size() + 1);
    std::copy(payload.begin(), payload.end(), event.data);
    events.push_back(event);
    return ROCKET_OK;
  });
}
void system_claim(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index),
               name = text(rdram, static_cast<std::uint32_t>(ctx->r4), 96);
    const auto &client = clients.at(id);
    if (!client.requested || !client.systems.contains(name))
      return ROCKET_INVALID;
    const auto found = system_owners.find(name);
    if (found != system_owners.end() && found->second != id)
      return ROCKET_CONFLICT;
    system_owners[name] = id;
    return ROCKET_OK;
  });
}
void system_release(std::uint8_t *rdram, recomp_context *ctx,
                    std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index),
               name = text(rdram, static_cast<std::uint32_t>(ctx->r4), 96);
    const auto found = system_owners.find(name);
    if (found == system_owners.end() || found->second != id)
      return ROCKET_NOT_FOUND;
    system_owners.erase(found);
    return ROCKET_OK;
  });
}
void hud_submit(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    const auto address = static_cast<std::uint32_t>(ctx->r4),
               count = static_cast<std::uint32_t>(ctx->r5);
    auto &client = clients.at(id);
    if (!in_callback || !in_gameplay || !client.enabled)
      return ROCKET_UNAVAILABLE;
    if (count > 64)
      return ROCKET_LIMIT;
    if (count && !range(address, count * sizeof(RocketHudItem), true))
      return ROCKET_INVALID;
    std::size_t total = count;
    for (const auto &[other, c] : clients)
      if (other != id)
        total += c.hud.size();
    if (total > 256)
      return ROCKET_LIMIT;
    std::vector<RocketHudItem> items;
    items.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
      const auto p = address + i * sizeof(RocketHudItem);
      if (word(rdram, p) != 2 || word(rdram, p + 4) < sizeof(RocketHudItem) ||
          word(rdram, p + 8) > ROCKET_HUD_RECTANGLE)
        return ROCKET_INVALID;
      RocketHudItem item{};
      item.api = 2;
      item.size = sizeof(item);
      item.kind = word(rdram, p + 8);
      item.rgba = word(rdram, p + 12);
      item.x = number(rdram, p + 16);
      item.y = number(rdram, p + 20);
      item.width = number(rdram, p + 24);
      item.height = number(rdram, p + 28);
      for (auto value : {item.x, item.y, item.width, item.height})
        if (!std::isfinite(value) || std::fabs(value) > 4096)
          return ROCKET_INVALID;
      const auto label = text(rdram, p + 32, 160);
      std::memcpy(item.text, label.c_str(), label.size() + 1);
      items.push_back(item);
    }
    client.hud = std::move(items);
    return ROCKET_OK;
  });
}
void sound_play(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&]() -> std::uint32_t {
    auto &client = clients.at(owner(index));
    if (!in_callback || !in_gameplay || !client.enabled || ctx->r4 < 0 ||
        ctx->r4 >= 162 || ctx->r5 < 0 || ctx->r5 > 128 || ctx->r6 < 0 ||
        ctx->r6 > 128)
      return 0;
    for (auto it = client.sounds.begin(); it != client.sounds.end();) {
      auto ask = *ctx;
      ask.r4 = *it;
      get_function(static_cast<std::int32_t>(0x80005AB4U))(rdram, &ask);
      if (ask.r2 == 0)
        it = client.sounds.erase(it);
      else
        ++it;
    }
    if (client.sounds.size() >= 32)
      return 0;
    auto native = *ctx;
    native.r7 = ctx->r6;
    native.r6 = ctx->r5;
    native.r5 = 0;
    get_function(static_cast<std::int32_t>(0x80091AF8U))(rdram, &native);
    const auto handle = static_cast<std::uint32_t>(native.r2);
    if (handle)
      client.sounds.insert(handle);
    return handle;
  });
  if (ctx->r2 < 0)
    ctx->r2 = 0;
}
void sound_stop(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    auto &client = clients.at(owner(index));
    const auto handle = static_cast<std::uint32_t>(ctx->r4);
    if (!in_callback)
      return ROCKET_UNAVAILABLE;
    if (!client.sounds.erase(handle))
      return ROCKET_NOT_FOUND;
    auto native = *ctx;
    native.r5 = 0;
    get_function(static_cast<std::int32_t>(0x80005A08U))(rdram, &native);
    return ROCKET_OK;
  });
}
std::vector<std::uint8_t> resource_data(const std::string &id,
                                        const std::string &name) {
  const auto handle = services.open(id, name);
  struct Close {
    std::string id;
    std::uint32_t handle;
    ~Close() { services.close(id, handle); }
  } close{id, handle};
  return services.read(id, handle, 0,
                       static_cast<std::uint32_t>(services.size(id, handle)));
}
template <class T> T packet(std::uint8_t *rdram, std::uint32_t address) {
  if (!range(address, sizeof(T), true) || word(rdram, address) != 2 ||
      word(rdram, address + 4) < sizeof(T))
    throw std::runtime_error("Invalid SDK packet.");
  T value;
  for (unsigned i = 0; i < sizeof(T) / 4; ++i) {
    const auto field = word(rdram, address + i * 4);
    std::memcpy(reinterpret_cast<std::uint8_t *>(&value) + i * 4, &field, 4);
  }
  return value;
}
template <class T>
void write_packet(std::uint8_t *rdram, std::uint32_t address, const T &value) {
  if (!range(address, sizeof(T), true))
    throw std::runtime_error("Invalid SDK output.");
  for (unsigned i = 0; i < sizeof(T) / 4; ++i) {
    std::uint32_t field;
    std::memcpy(&field, reinterpret_cast<const std::uint8_t *>(&value) + i * 4,
                4);
    put(rdram, address + i * 4, field);
  }
}
bool active(const std::string &id) {
  return in_callback && in_gameplay && clients.at(id).enabled;
}
void audio_load(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    if (!active(id))
      return 0U;
    return audio().load(
        id, resource_data(
                id, text(rdram, static_cast<std::uint32_t>(ctx->r4), 241)));
  });
  if (ctx->r2 < 0)
    ctx->r2 = 0;
}
void audio_play(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    if (!active(id))
      return 0U;
    return audio().play(id, packet<RocketAudioPlay>(
                                rdram, static_cast<std::uint32_t>(ctx->r4)));
  });
  if (ctx->r2 < 0)
    ctx->r2 = 0;
}
void audio_stop(std::uint8_t *, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    return audio().stop(owner(index), static_cast<std::uint32_t>(ctx->r4))
               ? ROCKET_OK
               : ROCKET_NOT_FOUND;
  });
}
void audio_playing(std::uint8_t *, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    return audio().playing(owner(index), static_cast<std::uint32_t>(ctx->r4))
               ? 1
               : 0;
  });
}
void mesh_load(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    if (!active(id))
      return 0U;
    return world.mesh(
        id, resource_data(
                id, text(rdram, static_cast<std::uint32_t>(ctx->r4), 241)));
  });
  if (ctx->r2 < 0)
    ctx->r2 = 0;
}
void actor_create(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    if (!active(id))
      return 0U;
    return world.create(id, packet<RocketActorState>(
                                rdram, static_cast<std::uint32_t>(ctx->r4)));
  });
  if (ctx->r2 < 0)
    ctx->r2 = 0;
}
void actor_pose(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    if (!active(id))
      return ROCKET_UNAVAILABLE;
    return world.pose(id, packet<RocketActorPose>(
                              rdram, static_cast<std::uint32_t>(ctx->r4)))
               ? ROCKET_OK
               : ROCKET_NOT_FOUND;
  });
}
void actor_read(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    auto state =
        packet<RocketActorState>(rdram, static_cast<std::uint32_t>(ctx->r5));
    if (!world.read(id, static_cast<std::uint32_t>(ctx->r4), state))
      return ROCKET_NOT_FOUND;
    write_packet(rdram, static_cast<std::uint32_t>(ctx->r5), state);
    return ROCKET_OK;
  });
}
void actor_update(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    if (!active(id))
      return ROCKET_UNAVAILABLE;
    return world.update(id, static_cast<std::uint32_t>(ctx->r4),
                        packet<RocketActorState>(
                            rdram, static_cast<std::uint32_t>(ctx->r5)))
               ? ROCKET_OK
               : ROCKET_NOT_FOUND;
  });
}
void actor_remove(std::uint8_t *, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    return world.remove(owner(index), static_cast<std::uint32_t>(ctx->r4))
               ? ROCKET_OK
               : ROCKET_NOT_FOUND;
  });
}
void scene_load(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    if (!active(id))
      return ROCKET_UNAVAILABLE;
    const auto data = resource_data(
        id, text(rdram, static_cast<std::uint32_t>(ctx->r4), 241));
    if (data.size() > 1024 * 1024)
      return ROCKET_LIMIT;
    const auto scene = Json::parse(data);
    world.scene(id, scene,
                [&](const auto &name) { return resource_data(id, name); });
    return ROCKET_OK;
  });
}
void scene_clear(std::uint8_t *, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    world.clear(owner(index));
    return ROCKET_OK;
  });
}
void scene_load_at(std::uint8_t *rdram, recomp_context *ctx,
                   std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    if (!active(id))
      return ROCKET_UNAVAILABLE;
    const auto origin = static_cast<std::uint32_t>(ctx->r5);
    if (!range(origin, 12, true))
      return ROCKET_INVALID;
    const RocketVec3 offset{number(rdram, origin), number(rdram, origin + 4),
                            number(rdram, origin + 8)};
    const auto data = resource_data(
        id, text(rdram, static_cast<std::uint32_t>(ctx->r4), 241));
    if (data.size() > 1024 * 1024)
      return ROCKET_LIMIT;
    world.scene(
        id, Json::parse(data),
        [&](const auto &name) { return resource_data(id, name); }, offset);
    return ROCKET_OK;
  });
}
void actor_find(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    return world.find(owner(index),
                      text(rdram, static_cast<std::uint32_t>(ctx->r4), 97));
  });
  if (ctx->r2 < 0)
    ctx->r2 = 0;
}
void collision_move(std::uint8_t *rdram, recomp_context *ctx,
                    std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    if (!active(id))
      return ROCKET_UNAVAILABLE;
    const auto address = static_cast<std::uint32_t>(ctx->r4);
    auto motion = packet<RocketMotion>(rdram, address);
    world.move(id, motion);
    write_packet(rdram, address, motion);
    return ROCKET_OK;
  });
}
void native_render(std::uint8_t *, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    if (!active(id))
      return ROCKET_UNAVAILABLE;
    const auto claim = system_owners.find("world.render");
    if (claim == system_owners.end() || claim->second != id)
      return ROCKET_CONFLICT;
    if (ctx->r4 != 0 && ctx->r4 != 1)
      return ROCKET_INVALID;
    clients.at(id).native_visible = ctx->r4 != 0;
    return ROCKET_OK;
  });
}
void native_music(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    if (!active(id))
      return ROCKET_UNAVAILABLE;
    const auto claim = system_owners.find("audio.music");
    if (claim == system_owners.end() || claim->second != id)
      return ROCKET_CONFLICT;
    if (ctx->r4 < 0 || ctx->r4 > 100)
      return ROCKET_INVALID;
    auto &client = clients.at(id);
    if (client.music_original < 0) {
      std::uint16_t volume;
      std::memcpy(&volume, rdram + ((0x800BD2EA & 0x3FFFFFFF) ^ 2U), 2);
      client.music_original = volume;
    }
    client.music_last = client.music_original * static_cast<int>(ctx->r4) / 100;
    auto native = *ctx;
    native.r4 = 2;
    native.r5 = client.music_last;
    get_function(static_cast<std::int32_t>(0x8000546CU))(rdram, &native);
    return ROCKET_OK;
  });
}
void command_take(std::uint8_t *rdram, recomp_context *ctx, std::size_t index) {
  guarded(ctx, [&] {
    const auto id = owner(index);
    if (!active(id))
      return 0U;
    auto &commands = clients.at(id).commands;
    const auto found =
        commands.find(text(rdram, static_cast<std::uint32_t>(ctx->r4), 97));
    if (found == commands.end())
      return 0U;
    const auto count = found->second;
    found->second = 0;
    return count;
  });
  if (ctx->r2 < 0)
    ctx->r2 = 0;
}
void collision_ray(std::uint8_t *rdram, recomp_context *ctx,
                   std::size_t index) {
  guarded(ctx, [&]() -> int {
    const auto ray =
        packet<RocketRay>(rdram, static_cast<std::uint32_t>(ctx->r4));
    auto hit = packet<RocketHit>(rdram, static_cast<std::uint32_t>(ctx->r5));
    if (!world.ray(owner(index), ray, hit))
      return 0;
    write_packet(rdram, static_cast<std::uint32_t>(ctx->r5), hit);
    return 1;
  });
}
} // namespace
void register_exports() {
  using namespace recomp::overlays;
  register_base_export("rocket_sdk_version", version);
  register_base_export("rocket_sdk_module", module);
  register_ext_base_export("rocket_sdk_register", register_client);
  register_ext_base_export("rocket_sdk_enabled", enabled);
  register_ext_base_export("rocket_sdk_log", log);
  register_ext_base_export("rocket_input_action", input_action);
  register_ext_base_export("rocket_resource_open", resource_open);
  register_ext_base_export("rocket_resource_size", resource_size);
  register_ext_base_export("rocket_resource_read", resource_read);
  register_ext_base_export("rocket_resource_close", resource_close);
  register_ext_base_export("rocket_save_write", save_write);
  register_ext_base_export("rocket_save_read", save_read);
  register_base_export("rocket_player_handle", player);
  register_base_export("rocket_object_observe", object_observe);
  register_base_export("rocket_object_read", object_read);
  register_base_export("rocket_object_position", position);
  register_base_export("rocket_object_velocity", velocity);
  register_ext_base_export("rocket_event_subscribe", event_subscribe);
  register_ext_base_export("rocket_event_emit", event_emit);
  register_ext_base_export("rocket_system_claim", system_claim);
  register_ext_base_export("rocket_system_release", system_release);
  register_ext_base_export("rocket_hud_submit", hud_submit);
  register_ext_base_export("rocket_sound_play", sound_play);
  register_ext_base_export("rocket_sound_stop", sound_stop);
  register_ext_base_export("rocket_audio_load", audio_load);
  register_ext_base_export("rocket_audio_play", audio_play);
  register_ext_base_export("rocket_audio_stop", audio_stop);
  register_ext_base_export("rocket_audio_playing", audio_playing);
  register_ext_base_export("rocket_mesh_load", mesh_load);
  register_ext_base_export("rocket_actor_create", actor_create);
  register_ext_base_export("rocket_actor_pose", actor_pose);
  register_ext_base_export("rocket_actor_read", actor_read);
  register_ext_base_export("rocket_actor_update", actor_update);
  register_ext_base_export("rocket_actor_remove", actor_remove);
  register_ext_base_export("rocket_scene_load", scene_load);
  register_ext_base_export("rocket_scene_load_at", scene_load_at);
  register_ext_base_export("rocket_actor_find", actor_find);
  register_ext_base_export("rocket_collision_move", collision_move);
  register_ext_base_export("rocket_native_render", native_render);
  register_ext_base_export("rocket_native_music_gain", native_music);
  register_ext_base_export("rocket_command_take", command_take);
  register_ext_base_export("rocket_scene_clear", scene_clear);
  register_ext_base_export("rocket_collision_ray", collision_ray);
}
void prepare() {
  std::lock_guard lock(mutex);
  clients.clear();
  order.clear();
  objects.clear();
  events.clear();
  system_owners.clear();
  scratch = 0;
  scene = 0;
  ticks = 0;
  scene_root = 0;
  previous_clock = -1;
  in_gameplay = false;
  current = {};
  world.reset();
  audio().reset();
  const auto snapshot = library().snapshot();
  std::vector<Package> packages;
  rocket::input::clear_mod_actions();
  for (const auto &active : snapshot.active.at("packages")) {
    const auto it = std::find_if(
        snapshot.packages.begin(), snapshot.packages.end(), [&](const auto &p) {
          return p.hash == active.at("hash").get<std::string>();
        });
    if (it == snapshot.packages.end())
      continue;
    packages.push_back(*it);
    // API 1 packages are mounted for resources/save ownership but never gain
    // callbacks or activation behavior without making an SDK 2 registration.
    Client client;
    client.requested = active.value("enabled", true);
    client.managed = managed_activation(*it);
    client.api2 = it->metadata.value("api", 1) == 2;
    if (client.api2)
      for (const auto &command : it->metadata.value("commands", Json::array()))
        client.commands.emplace(command.at("id").get<std::string>(), 0);
    if (client.api2)
      for (const auto &name : it->metadata.value("systems", Json::array()))
        client.systems.insert(name.get<std::string>());
    if (!client.api2)
      for (const auto &name :
           it->metadata.value("exclusive_resources", Json::array()))
        system_owners[name.get<std::string>()] = it->id;
    std::vector<rocket::input::ModAction> actions;
    const auto definitions =
        client.api2 ? it->metadata.value("input_actions", Json::object())
                    : Json::object();
    const auto bindings = active.value("bindings", Json::object());
    for (auto definition = definitions.begin(); definition != definitions.end();
         ++definition) {
      const auto assigned = bindings.value(definition.key(), Json::object());
      actions.push_back(
          {definition.key(),
           assigned.value("keyboard", definition.value().value("keyboard", -1)),
           assigned.value("controller",
                          definition.value().value("controller", -1)),
           static_cast<std::uint16_t>(
               assigned.value("n64", definition.value().value("n64", 0)))});
      client.input_edges.emplace(definition.key(), std::pair{0U, 0U});
    }
    clients.emplace(it->id, client);
    order.push_back(it->id);
    rocket::input::register_mod_actions(it->id, actions);
  }
  services.begin(packages, library().active_save_path());
}
void ready(std::uint8_t *rdram) {
  std::lock_guard lock(mutex);
  if (std::none_of(clients.begin(), clients.end(),
                   [](const auto &value) { return value.second.api2; }))
    return;
  const auto storage = recomp::alloc(rdram, 0x10000);
  if (storage)
    scratch = static_cast<std::uint32_t>(static_cast<std::uint8_t *>(storage) -
                                         rdram) +
              0x80000000U;
}
void request_enabled(const std::string &id, bool value) {
  std::lock_guard lock(mutex);
  const auto it = clients.find(id);
  if (it != clients.end() && it->second.managed) {
    it->second.requested = value;
    if (!value)
      rocket::input::set_mod_actions_enabled(id, false);
  }
}
void settings_changed(const std::string &id) {
  std::lock_guard lock(mutex);
  const auto it = clients.find(id);
  if (it != clients.end())
    it->second.dirty = true;
}
bool request_command(const std::string &id, const std::string &command) {
  std::lock_guard lock(mutex);
  const auto found = clients.find(id);
  if (found == clients.end() || !found->second.registered ||
      !found->second.enabled || !found->second.requested)
    return false;
  auto &commands = found->second.commands;
  const auto target = commands.find(command);
  if (target == commands.end() || target->second == UINT32_MAX)
    return false;
  ++target->second;
  return true;
}
Status status(const std::string &id) {
  std::lock_guard lock(mutex);
  const auto it = clients.find(id);
  if (it == clients.end())
    return {};
  const auto &c = it->second;
  return {c.registered, c.enabled, c.requested, c.ticks};
}
std::vector<RocketHudItem> hud_snapshot() {
  std::lock_guard lock(mutex);
  std::vector<RocketHudItem> items;
  for (const auto &id : order) {
    const auto &client = clients.at(id);
    if (client.enabled)
      items.insert(items.end(), client.hud.begin(), client.hud.end());
  }
  return items;
}
void tick(std::uint8_t *rdram, const recomp_context &ctx) {
  std::lock_guard lock(mutex);
  if (scratch == 0 || in_callback)
    return;
  const auto root = word(rdram, 0x8009F094),
             player_address = word(rdram, 0x800AAF5C);
  const bool gameplay = word(rdram, 0x800ABCD8) == 4 &&
                        word(rdram, 0x800AC2E4) == 0 &&
                        word(rdram, 0x800AF5F0) == 0xFFFFFFFFU &&
                        original_range(player_address, 0x184) && root != 0;
  const auto clock = number(rdram, 0x8009FE14);
  const bool changed = in_gameplay && (!gameplay || root != scene_root);
  if (changed) {
    in_gameplay = false;
    objects.clear();
    current.player = 0;
    current.delta_time = 0;
    current.state = gameplay ? ROCKET_LOADING : ROCKET_FRONTEND;
    for (const auto &id : order) {
      auto &client = clients.at(id);
      if (client.enabled) {
        call(rdram, ctx, client.callbacks[2], client);
        if (client.api2)
          cleanup(rdram, ctx, id);
      }
    }
    objects.clear();
    events.clear();
    in_gameplay = false;
    previous_clock = -1;
  }
  const bool entered = gameplay && !in_gameplay;
  if (entered) {
    ++scene;
    scene_root = root;
    in_gameplay = true;
    previous_clock = -1;
  }
  const float delta = gameplay && previous_clock >= 0 && std::isfinite(clock)
                          ? std::clamp(clock - previous_clock, 0.0F, 0.1F)
                          : 0;
  previous_clock = gameplay ? clock : -1;
  audio().pause(!gameplay || delta == 0);
  // A native pointer has no universal destruction notification. Native
  // observation handles are deliberately scoped to one update boundary.
  objects.clear();
  current = {2,
             sizeof(RocketTick),
             static_cast<RocketSdkU32>(
                 gameplay
                     ? (delta == 0 && !entered ? ROCKET_PAUSED : ROCKET_PLAYING)
                     : ROCKET_FRONTEND),
             scene,
             ++ticks,
             delta,
             gameplay ? observe(rdram, player_address) : 0};
  auto pending_events = std::move(events);
  events.clear();
  for (const auto &id : order) {
    auto &client = clients.at(id);
    if (!client.registered)
      continue;
    if (!client.notified || client.requested != client.enabled) {
      client.enabled = client.requested;
      client.notified = true;
      rocket::input::set_mod_actions_enabled(id, client.enabled && gameplay);
      call(rdram, ctx, client.callbacks[client.enabled ? 3 : 4], client,
           !client.enabled);
      if (!client.enabled) {
        services.release(id);
        cleanup(rdram, ctx, id);
      } else if (gameplay && !entered)
        call(rdram, ctx, client.callbacks[1], client);
    }
    if (client.dirty) {
      client.dirty = false;
      call(rdram, ctx, client.callbacks[5], client);
    }
    if (!client.enabled)
      continue;
    rocket::input::set_mod_actions_enabled(id, gameplay);
    if (entered)
      call(rdram, ctx, client.callbacks[1], client);
    if (gameplay) {
      world.step(id, delta);
      for (const auto &event : pending_events) {
        const auto subscription = client.subscriptions.find(event.name);
        if (subscription != client.subscriptions.end()) {
          // Strings/bytes are byte-swapped separately from integer words.
          auto packet = event;
          for (unsigned i = 0; i < sizeof(packet.name); i += 4)
            std::reverse(packet.name + i, packet.name + i + 4);
          for (unsigned i = 0; i < sizeof(packet.data); i += 4)
            std::reverse(packet.data + i, packet.data + i + 4);
          call_packet(rdram, ctx, subscription->second, &packet, sizeof(packet),
                      client);
        }
      }
      call(rdram, ctx, client.callbacks[0], client);
      ++client.ticks;
    }
  }
}
std::uint32_t render(std::uint8_t *rdram) {
  std::lock_guard lock(mutex);
  static int reported = 0;
  const auto original_queue = word(rdram, 0x800AF300);
  bool replacing = false;
  unsigned draws = 0;
  if (in_gameplay) {
    const auto claimant = system_owners.find("world.render");
    if (claimant != system_owners.end()) {
      const auto found = clients.find(claimant->second);
      if (found != clients.end() && found->second.enabled &&
          !found->second.native_visible) {
        replacing = true;
        put(rdram, 0x800AF300, 0x80600000);
      }
    }
    draws = render_world(world, rdram);
    if (draws == 0 && replacing) {
      put(rdram, 0x800AF300, original_queue);
    }
  }
  const int state = replacing ? (draws ? 1 : 2) : 0;
  if (state != reported) {
    if (state == 1)
      std::fprintf(stderr,
                   "[mod-sdk] Custom world active: %u actors submitted.\n",
                   draws);
    else if (state == 2)
      std::fprintf(stderr, "[mod-sdk] Custom world submitted no draws; keeping "
                           "native scenery visible.\n");
    reported = state;
  }
  return word(rdram, 0x800AF300);
}
} // namespace rocket::mods::sdk
extern "C" void rocket_sdk_tick(std::uint8_t *rdram, recomp_context *ctx) {
  rocket::mods::sdk::tick(rdram, *ctx);
}
extern "C" void rocket_sdk_render(std::uint8_t *rdram, recomp_context *ctx) {
  // The checked 8008B7F8 hook runs after LW v1,D_800AF300. Refresh that
  // already-loaded cursor before the game's subtraction/count calculation.
  ctx->r3 = static_cast<gpr>(
      static_cast<std::int32_t>(rocket::mods::sdk::render(rdram)));
}
