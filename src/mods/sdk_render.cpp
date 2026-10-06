#include "sdk_render.hpp"
#include "presentation_identity.hpp"
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rocket::mods::sdk {
namespace {
constexpr std::uint32_t context = 0x800A5DA8, queue_base = 0x80600000,
                        queue_end = queue_base + 32768 * 24,
                        sort_base = 0x80700000, bank_size = 0x20000;
static_assert(queue_end + 2 * bank_size == sort_base);
bool range(std::uint32_t p, std::uint32_t size) {
  return p >= 0x80000400 && p < 0x80800000 && size <= 0x80800000 - p &&
         (p & 3) == 0;
}
std::uint32_t word(std::uint8_t *ram, std::uint32_t p) {
  std::uint32_t value;
  std::memcpy(&value, ram + (p & 0x7FFFFF), 4);
  return value;
}
void put(std::uint8_t *ram, std::uint32_t p, std::uint32_t value) {
  std::memcpy(ram + (p & 0x7FFFFF), &value, 4);
}
void trace(std::uint8_t *ram, std::uint32_t task,
           const std::vector<World::Draw> &draws) {
  static const bool enabled = [] {
    const char *value = std::getenv("ROCKET_SDK_RENDER_TRACE");
    return value && std::strcmp(value, "1") == 0;
  }();
  static unsigned samples = 0;
  if (!enabled || draws.size() < 2 || samples >= 240)
    return;
  const auto sample = ++samples;
  if (sample != 1 && sample != 30 && sample != 120 && sample != 240)
    return;
  std::fprintf(stderr, "[mod-sdk-matrix] sample=%u task=%08x actors=%zu\n",
               sample, task, draws.size());
  for (unsigned offset : {0x18U, 0x58U}) {
    std::fprintf(stderr, "[mod-sdk-matrix] %s",
                 offset == 0x18 ? "projection" : "view");
    for (unsigned i = 0; i < 16; ++i) {
      const auto integer = word(ram, task + offset + (i / 2) * 4);
      const auto fraction = word(ram, task + offset + 32 + (i / 2) * 4);
      const auto value = (i & 1) ? (integer << 16) | (fraction & 0xFFFF)
                                 : (integer & 0xFFFF0000) | (fraction >> 16);
      std::fprintf(stderr, " %.5f", static_cast<std::int32_t>(value) / 65536.0);
    }
    std::fprintf(stderr, "\n");
  }
  for (const auto &draw : draws)
    std::fprintf(stderr,
                 "[mod-sdk-matrix] actor=%u pos=%.3f,%.3f,%.3f "
                 "scale=%.3f,%.3f,%.3f colour=%08x\n",
                 draw.handle, draw.state.position.x, draw.state.position.y,
                 draw.state.position.z, draw.state.scale.x, draw.state.scale.y,
                 draw.state.scale.z, draw.mesh->vertices.front().rgba);
}
bool matrix(std::uint8_t *ram, std::uint32_t address, const RocketActorState &s,
            const std::array<float, 4> &q) {
  const float c = std::cos(s.yaw), n = std::sin(s.yaw);
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  const float rotation[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w),
                             2 * (x * z - y * w),     2 * (x * y - z * w),
                             1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                             2 * (x * z + y * w),     2 * (y * z - x * w),
                             1 - 2 * (x * x + y * y)};
  float values[16] = {};
  // Rocket's projection already includes the camera view matrix. Its native
  // coordinates are sixteen times the public gameplay coordinates.
  const float scale[3] = {s.scale.x * 16, s.scale.y * 16, s.scale.z * 16};
  for (unsigned i = 0; i < 3; ++i) {
    values[i * 4] = (rotation[i * 3] * c - rotation[i * 3 + 1] * n) * scale[i];
    values[i * 4 + 1] =
        (rotation[i * 3] * n + rotation[i * 3 + 1] * c) * scale[i];
    values[i * 4 + 2] = rotation[i * 3 + 2] * scale[i];
  }
  values[12] = s.position.x * 16;
  values[13] = s.position.y * 16;
  values[14] = s.position.z * 16;
  values[15] = 1;
  for (float value : values)
    if (!std::isfinite(value) || value < -32768 || value >= 32768)
      return false;
  for (unsigned i = 0; i < 16; i += 2) {
    const auto a = static_cast<std::uint32_t>(
                   static_cast<std::int32_t>(values[i] * 65536.0F)),
               b = static_cast<std::uint32_t>(
                   static_cast<std::int32_t>(values[i + 1] * 65536.0F));
    put(ram, address + i * 2, (a & 0xFFFF0000) | (b >> 16));
    put(ram, address + 32 + i * 2, (a << 16) | (b & 0xFFFF));
  }
  return true;
}
} // namespace
unsigned render_world(const World &world, std::uint8_t *ram) {
  if (!ram)
    return 0;
  const auto draws = world.draws();
  if (draws.empty())
    return 0;
  const auto start = word(ram, context + 4), size = word(ram, context),
             head = word(ram, context + 8), task = word(ram, 0x800A5DBC);
  const auto tail = word(ram, context + 12);
  auto queue = word(ram, 0x800AF300);
  if (size > 0x20000 || !range(start, size) || !range(task, 0x128) ||
      head < start || head >= start + size || tail <= head ||
      tail > start + size || queue < queue_base || queue >= queue_end ||
      (queue - queue_base) % 24)
    return 0;
  std::uint16_t bank;
  std::memcpy(&bank, ram + (((task + 0x120) & 0x7FFFFF) ^ 2U), 2);
  if (bank > 1)
    return 0;
  trace(ram, task, draws);
  // The expanded entry queue ends at 806C0000 and sort scratch starts at
  // 80700000. Keep custom draw data in this reserved gap, with one bank per
  // native GfxTask. The retail 32 KiB arena remains available to its renderer.
  auto cursor = queue_end + bank * bank_size;
  const auto end = cursor + bank_size;
  const auto native_count = (queue - queue_base) / 24;
  unsigned count = 0;
  for (const auto &draw : draws) {
    const auto &mesh = *draw.mesh;
    const auto triangles = static_cast<std::uint32_t>(mesh.triangles.size());
    const auto batches = (triangles + 9) / 10;
    const auto texture_bytes =
        static_cast<std::uint32_t>(mesh.texture.size() * 2);
    const auto commands = 6 + triangles + batches + (texture_bytes ? 7 : 0);
    const auto vertex_bytes = triangles * 3 * 16;
    const auto bytes =
        (64 + vertex_bytes + texture_bytes + commands * 8 + 63) & ~63U;
    // Each entry needs matrix/material/call commands in the native head,
    // plus room for the existing entries and HUD after this hook.
    const auto command_reserve = 4096 + native_count * 80 + (count + 1) * 64;
    if (bytes > end - cursor || command_reserve > tail - head ||
        queue + 24 > queue_end)
      break;
    if (!range(cursor, bytes))
      break;
    const auto model = cursor, vertices = cursor + 64,
               texture = vertices + vertex_bytes,
               list = texture + texture_bytes;
    if (!matrix(ram, model, draw.state, draw.orientation))
      continue;
    auto command = list;
    auto emit = [&](std::uint32_t a, std::uint32_t b) {
      put(ram, command, a);
      put(ram, command + 4, b);
      command += 8;
    };
    emit(0xE7000000, 0); // RDP pipe sync before changing the combiner.
    emit(0xD9FDFFFF,
         0x204); // Unlit, shaded, smoothly interpolated vertex colours.
    if (texture_bytes) {
      for (unsigned i = 0; i < mesh.texture.size(); i += 2)
        put(ram, texture + i * 2,
            (static_cast<std::uint32_t>(mesh.texture[i]) << 16) |
                mesh.texture[i + 1]);
      // gDPLoadTextureBlock: RGBA16, clamp, no palette/mipmaps. At most 2 KiB
      // TMEM.
      emit(0xFD100000, texture & 0x7FFFFF);
      emit(0xF5100000, 0x07080200);
      emit(0xE6000000, 0);
      emit(0xF3000000, 0x07000000 | ((mesh.width * mesh.height - 1) << 12) |
                           ((2048 + (mesh.width / 4) - 1) / (mesh.width / 4)));
      emit(0xE7000000, 0);
      emit(0xF5100000 | ((mesh.width / 4) << 9), 0x00080200);
      emit(0xF2000000, ((mesh.width - 1) * 4 << 12) | ((mesh.height - 1) * 4));
      emit(0xD7000002, 0xFFFFFFFF);
      emit(0xFC121824, 0xFF33FFFF); // G_CC_MODULATERGBA.
    } else {
      emit(0xD7000000, 0);          // Texture off.
      emit(0xFCFFFFFF, 0xFFFE793C); // G_CC_SHADE in both cycles.
    }
    for (unsigned first = 0; first < triangles; first += 10) {
      const auto batch = std::min(10U, triangles - first), length = batch * 3;
      const auto buffer = vertices + first * 3 * 16;
      for (unsigned j = 0; j < batch; ++j)
        for (unsigned corner = 0; corner < 3; ++corner) {
          const auto &v = mesh.vertices[mesh.triangles[first + j][corner]];
          const auto p = buffer + (j * 3 + corner) * 16;
          const auto x = static_cast<std::uint16_t>(
                         static_cast<std::int16_t>(std::lround(v.position.x))),
                     y = static_cast<std::uint16_t>(
                         static_cast<std::int16_t>(std::lround(v.position.y))),
                     z = static_cast<std::uint16_t>(
                         static_cast<std::int16_t>(std::lround(v.position.z)));
          put(ram, p, (static_cast<std::uint32_t>(x) << 16) | y);
          put(ram, p + 4, static_cast<std::uint32_t>(z) << 16);
          const auto s = static_cast<std::uint16_t>(static_cast<std::int16_t>(
                         std::lround(v.u * mesh.width * 32))),
                     t = static_cast<std::uint16_t>(static_cast<std::int16_t>(
                         std::lround(v.v * mesh.height * 32)));
          put(ram, p + 8, (static_cast<std::uint32_t>(s) << 16) | t);
          put(ram, p + 12, v.rgba);
        }
      emit(0x01000000 | (length << 12) | (length << 1), buffer & 0x7FFFFF);
      for (unsigned j = 0; j < batch; ++j) {
        const auto a = j * 6;
        emit(0x05000000 | (a << 16) | ((a + 2) << 8) | (a + 4), 0);
      }
    }
    emit(0xD9FFFFFF,
         0x00020000); // Native material lists select their own lighting.
    emit(0xDF000000, 0);
    put(ram, queue, list);
    put(ram, queue + 4, model);
    put(ram, queue + 8, 0); // Camera view is already in the projection stack.
    put(ram, queue + 12, std::bit_cast<std::uint32_t>(0.0F));
    put(ram, queue + 16, 0x11010101);
    put(ram, queue + 20, 0xFF000000);
    rocket::presentation::mod_matrix(model, draw.handle);
    cursor += bytes;
    queue += 24;
    ++count;
  }
  if (count) {
    put(ram, 0x800AF300, queue);
  }
  return count;
}
} // namespace rocket::mods::sdk
