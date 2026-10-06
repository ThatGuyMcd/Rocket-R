#include "sdk_world.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace rocket::mods::sdk {
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
float component(const RocketVec3 &v, unsigned i) {
  return i == 0 ? v.x : i == 1 ? v.y : v.z;
}
void component(RocketVec3 &v, unsigned i, float value) {
  if (i == 0)
    v.x = value;
  else if (i == 1)
    v.y = value;
  else
    v.z = value;
}
bool finite(const RocketVec3 &v, float limit) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
         std::fabs(v.x) <= limit && std::fabs(v.y) <= limit &&
         std::fabs(v.z) <= limit;
}
RocketVec3 vector(const Json &value) {
  require(value.is_array() && value.size() == 3,
          "Scene vectors need three numbers.");
  RocketVec3 result{value[0].get<float>(), value[1].get<float>(),
                    value[2].get<float>()};
  require(finite(result, 30000),
          "Scene vector is outside the supported range.");
  return result;
}
} // namespace
Mesh decode_mesh(std::span<const std::uint8_t> bytes) {
  require(bytes.size() >= 16 && std::memcmp(bytes.data(), "RRM2", 4) == 0,
          "Not a Rocket mesh.");
  auto word = [&](std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           bytes[offset + 3];
  };
  const auto version = word(4), vertices = word(8), triangles = word(12);
  require((version == 1 || version == 2) && vertices > 0 && vertices <= 4096 &&
              triangles > 0 && triangles <= 4096,
          "Unsupported mesh version or size.");
  const unsigned header = version == 1 ? 16 : 24,
                 stride = version == 1 ? 16 : 24;
  require(bytes.size() >= header, "Truncated mesh header.");
  const unsigned width = version == 1 ? 0 : word(16),
                 height = version == 1 ? 0 : word(20);
  require(version == 1 || ((width == 8 || width == 16 || width == 32) &&
                           (height == 8 || height == 16 || height == 32)),
          "Texture size must be 8, 16 or 32 pixels on each side.");
  require(bytes.size() ==
              header + vertices * stride + triangles * 12 + width * height * 2,
          "Mesh payload has the wrong size.");
  Mesh mesh;
  mesh.vertices.reserve(vertices);
  mesh.triangles.reserve(triangles);
  for (unsigned i = 0; i < vertices; ++i) {
    const auto p = header + i * stride;
    MeshVertex v{{std::bit_cast<float>(word(p)),
                  std::bit_cast<float>(word(p + 4)),
                  std::bit_cast<float>(word(p + 8))},
                 word(p + 12)};
    require(finite(v.position, 32760),
            "Mesh coordinates must fit N64 vertices.");
    if (version == 2) {
      v.u = std::bit_cast<float>(word(p + 16));
      v.v = std::bit_cast<float>(word(p + 20));
      require(std::isfinite(v.u) && std::isfinite(v.v) &&
                  std::fabs(v.u) <= 16 && std::fabs(v.v) <= 16,
              "Mesh UV is outside the supported range.");
    }
    mesh.vertices.push_back(v);
  }
  for (unsigned i = 0; i < triangles; ++i) {
    const auto p = header + vertices * stride + i * 12;
    std::array<std::uint32_t, 3> t{word(p), word(p + 4), word(p + 8)};
    require(t[0] < vertices && t[1] < vertices && t[2] < vertices,
            "Mesh triangle index is outside the vertex list.");
    mesh.triangles.push_back(t);
  }
  mesh.width = width;
  mesh.height = height;
  const auto pixels = header + vertices * stride + triangles * 12;
  for (unsigned i = 0; i < width * height; ++i)
    mesh.texture.push_back(static_cast<std::uint16_t>(
        (bytes[pixels + i * 2] << 8) | bytes[pixels + i * 2 + 1]));
  return mesh;
}
std::uint32_t World::handle() {
  require(next_ != 0, "World handles exhausted.");
  return next_++;
}
void World::reset() {
  meshes_.clear();
  actors_.clear();
  boxes_.clear();
}
void World::clear(const std::string &owner) {
  for (auto it = actors_.begin(); it != actors_.end();)
    if (it->second.owner == owner)
      it = actors_.erase(it);
    else
      ++it;
  for (auto it = meshes_.begin(); it != meshes_.end();)
    if (it->second.owner == owner)
      it = meshes_.erase(it);
    else
      ++it;
  for (auto it = boxes_.begin(); it != boxes_.end();)
    if (it->second.owner == owner)
      it = boxes_.erase(it);
    else
      ++it;
}
std::uint32_t World::mesh(const std::string &owner,
                          std::span<const std::uint8_t> data) {
  require(meshes_.size() < 128, "Mesh count exceeds the session budget.");
  auto parsed = decode_mesh(data);
  std::size_t memory = parsed.vertices.size() * sizeof(MeshVertex) +
                       parsed.triangles.size() * 12 + parsed.texture.size() * 2;
  for (const auto &[id, m] : meshes_)
    memory += m.mesh.vertices.size() * sizeof(MeshVertex) +
              m.mesh.triangles.size() * 12 + m.mesh.texture.size() * 2;
  require(memory <= 16 * 1024 * 1024,
          "Meshes exceed the 16 MiB session budget.");
  const auto id = handle();
  meshes_.emplace(id, OwnedMesh{owner, std::move(parsed)});
  return id;
}
void World::validate(const std::string &owner,
                     const RocketActorState &s) const {
  const auto mesh = meshes_.find(s.mesh);
  require(s.api == 2 && s.size >= sizeof(s) && mesh != meshes_.end() &&
              mesh->second.owner == owner,
          "Invalid actor/mesh ownership.");
  require(s.visible <= 1 && finite(s.position, 30000) &&
              finite(s.velocity, 10000) && finite(s.scale, 100) &&
              s.scale.x > 0 && s.scale.y > 0 && s.scale.z > 0 &&
              std::isfinite(s.yaw),
          "Invalid actor transform.");
}
std::uint32_t World::create(const std::string &owner,
                            const RocketActorState &state) {
  validate(owner, state);
  require(actors_.size() < 256, "Actor count exceeds the session budget.");
  const auto id = handle();
  actors_.emplace(id, Actor{owner, state, {}});
  return id;
}
bool World::read(const std::string &owner, std::uint32_t id,
                 RocketActorState &s) const {
  const auto it = actors_.find(id);
  if (it == actors_.end() || it->second.owner != owner)
    return false;
  s = it->second.state;
  return true;
}
bool World::update(const std::string &owner, std::uint32_t id,
                   const RocketActorState &s) {
  const auto it = actors_.find(id);
  if (it == actors_.end() || it->second.owner != owner)
    return false;
  validate(owner, s);
  it->second.state = s;
  return true;
}
bool World::pose(const std::string &owner, const RocketActorPose &p) {
  const auto found = actors_.find(p.actor);
  if (found == actors_.end() || found->second.owner != owner)
    return false;
  require(p.api == 2 && p.size >= sizeof(p),
          "Invalid actor orientation packet.");
  const std::array<float, 4> q{p.x, p.y, p.z, p.w};
  float squared = 0;
  for (auto value : q) {
    require(std::isfinite(value) && std::fabs(value) <= 1000000,
            "Invalid actor quaternion.");
    squared += value * value;
  }
  require(squared > 1e-12F, "Actor quaternion is too short.");
  const float inverse = 1 / std::sqrt(squared);
  for (unsigned i = 0; i < 4; ++i)
    found->second.orientation[i] = q[i] * inverse;
  return true;
}
bool World::remove(const std::string &owner, std::uint32_t id) {
  const auto it = actors_.find(id);
  if (it == actors_.end() || it->second.owner != owner)
    return false;
  actors_.erase(it);
  return true;
}
void World::scene(const std::string &owner, const Json &scene,
                  const Loader &loader, const RocketVec3 &origin) {
  require(finite(origin, 30000), "Invalid scene origin.");
  auto translated = [&](RocketVec3 v) {
    v.x += origin.x;
    v.y += origin.y;
    v.z += origin.z;
    require(finite(v, 30000), "Translated scene exceeds the coordinate range.");
    return v;
  };
  require(scene.value("schema", 0) == 1, "Unsupported custom scene schema.");
  const auto meshes = scene.value("meshes", Json::object()),
             actors = scene.value("actors", Json::array()),
             boxes = scene.value("collision", Json::array());
  require(meshes.is_object() && meshes.size() <= 32 && actors.is_array() &&
              actors.size() <= 64 && boxes.is_array() && boxes.size() <= 256,
          "Custom scene exceeds its budget.");
  World candidate = *this;
  candidate.clear(owner);
  std::map<std::string, std::uint32_t> references;
  for (auto it = meshes.begin(); it != meshes.end(); ++it)
    references[it.key()] =
        candidate.mesh(owner, loader(it.value().get<std::string>()));
  for (const auto &actor : actors) {
    RocketActorState s{2,
                       sizeof(RocketActorState),
                       references.at(actor.at("mesh").get<std::string>()),
                       1,
                       {},
                       {},
                       {1, 1, 1},
                       0};
    s.position =
        translated(vector(actor.value("position", Json::array({0, 0, 0}))));
    s.velocity = vector(actor.value("velocity", Json::array({0, 0, 0})));
    s.scale = vector(actor.value("scale", Json::array({1, 1, 1})));
    s.yaw = actor.value("yaw", 0.0F);
    const auto name = actor.value("id", std::string());
    require(name.empty() ||
                (valid_id(name) && candidate.find(owner, name) == 0),
            "Invalid or duplicate scene actor ID.");
    const auto handle = candidate.create(owner, s);
    if (actor.contains("orientation")) {
      const auto &q = actor.at("orientation");
      require(q.is_array() && q.size() == 4,
              "Scene orientations need four quaternion components.");
      candidate.pose(owner,
                     RocketActorPose{2, sizeof(RocketActorPose), handle,
                                     q[0].get<float>(), q[1].get<float>(),
                                     q[2].get<float>(), q[3].get<float>()});
    }
    candidate.actors_.at(handle).name = name;
  }
  for (const auto &box : boxes) {
    const auto low = translated(vector(box.at("min"))),
               high = translated(vector(box.at("max")));
    require(low.x < high.x && low.y < high.y && low.z < high.z &&
                candidate.boxes_.size() < 1024,
            "Invalid collision box or box budget.");
    candidate.boxes_.emplace(candidate.handle(), Box{owner, low, high});
  }
  *this = std::move(candidate);
}
std::uint32_t World::find(const std::string &owner,
                          const std::string &name) const {
  if (name.empty())
    return 0;
  for (const auto &[id, a] : actors_)
    if (a.owner == owner && a.name == name)
      return id;
  return 0;
}
void World::move(const std::string &owner, RocketMotion &m) const {
  require(m.api == 2 && m.size >= sizeof(m) && finite(m.position, 30000) &&
              finite(m.displacement, 10000) && finite(m.half_extent, 1000) &&
              m.half_extent.x > 0 && m.half_extent.y > 0 && m.half_extent.z > 0,
          "Invalid character sweep.");
  const auto start = m.position;
  auto remaining = m.displacement;
  m.normal = {};
  m.grounded = 0;
  // Push a starting overlap to its nearest face before sweeping. This also
  // handles scene edits/teleports without trapping the character inside a box.
  for (unsigned iteration = 0; iteration < 8; ++iteration) {
    bool corrected = false;
    for (const auto &[id, b] : boxes_) {
      if (b.owner != owner)
        continue;
      const RocketVec3 low{b.low.x - m.half_extent.x, b.low.y - m.half_extent.y,
                           b.low.z - m.half_extent.z},
          high{b.high.x + m.half_extent.x, b.high.y + m.half_extent.y,
               b.high.z + m.half_extent.z};
      if (m.position.x <= low.x || m.position.x >= high.x ||
          m.position.y <= low.y || m.position.y >= high.y ||
          m.position.z <= low.z || m.position.z >= high.z)
        continue;
      float nearest = std::numeric_limits<float>::max();
      unsigned axis = 0;
      float boundary = 0, sign = 0;
      for (unsigned a = 0; a < 3; ++a) {
        const auto p = component(m.position, a), lo = component(low, a),
                   hi = component(high, a);
        if (p - lo < nearest) {
          nearest = p - lo;
          axis = a;
          boundary = lo - .01F;
          sign = -1;
        }
        if (hi - p < nearest) {
          nearest = hi - p;
          axis = a;
          boundary = hi + .01F;
          sign = 1;
        }
      }
      component(m.position, axis, boundary);
      m.normal = {};
      component(m.normal, axis, sign);
      if (axis == 2 && sign > 0)
        m.grounded = 1;
      corrected = true;
    }
    if (!corrected)
      break;
  }
  for (unsigned iteration = 0; iteration < 4; ++iteration) {
    float nearest = 1;
    RocketVec3 normal{};
    bool hit = false;
    for (const auto &[id, b] : boxes_) {
      if (b.owner != owner)
        continue;
      float enter = 0, leave = 1;
      RocketVec3 candidate{};
      bool valid = true;
      for (unsigned a = 0; a < 3; ++a) {
        const auto p = component(m.position, a), d = component(remaining, a),
                   low = component(b.low, a) - component(m.half_extent, a),
                   high = component(b.high, a) + component(m.half_extent, a);
        if (std::fabs(d) < 1e-8F) {
          if (p < low || p > high) {
            valid = false;
            break;
          }
          continue;
        }
        float first = (low - p) / d, last = (high - p) / d;
        const auto sign = d > 0 ? -1.0F : 1.0F;
        if (first > last)
          std::swap(first, last);
        if (first >= enter) {
          enter = first;
          candidate = {};
          component(candidate, a, sign);
        }
        leave = std::min(leave, last);
        if (enter > leave) {
          valid = false;
          break;
        }
      }
      if (valid && enter <= nearest && leave >= 0 &&
          (candidate.x || candidate.y || candidate.z)) {
        nearest = enter;
        normal = candidate;
        hit = true;
      }
    }
    m.position.x += remaining.x * nearest;
    m.position.y += remaining.y * nearest;
    m.position.z += remaining.z * nearest;
    if (!hit)
      break;
    m.normal = normal;
    if (normal.z > .5F)
      m.grounded = 1;
    m.position.x += normal.x * .01F;
    m.position.y += normal.y * .01F;
    m.position.z += normal.z * .01F;
    remaining.x *= 1 - nearest;
    remaining.y *= 1 - nearest;
    remaining.z *= 1 - nearest;
    const float into = remaining.x * normal.x + remaining.y * normal.y +
                       remaining.z * normal.z;
    remaining.x -= normal.x * into;
    remaining.y -= normal.y * into;
    remaining.z -= normal.z * into;
  }
  require(finite(m.position, 30000),
          "Character sweep left the supported coordinate range.");
  m.displacement = {m.position.x - start.x, m.position.y - start.y,
                    m.position.z - start.z};
}
bool World::ray(const std::string &owner, const RocketRay &ray,
                RocketHit &hit) const {
  require(ray.api == 2 && ray.size >= sizeof(ray) && finite(ray.start, 30000) &&
              finite(ray.end, 30000),
          "Invalid collision ray.");
  bool found = false;
  float nearest = 1;
  for (const auto &[id, box] : boxes_) {
    if (box.owner != owner)
      continue;
    float enter = 0, leave = 1;
    RocketVec3 normal{};
    bool valid = true;
    for (unsigned axis = 0; axis < 3; ++axis) {
      const float start = component(ray.start, axis),
                  direction = component(ray.end, axis) - start,
                  low = component(box.low, axis),
                  high = component(box.high, axis);
      if (std::fabs(direction) < 1e-8F) {
        if (start < low || start > high) {
          valid = false;
          break;
        }
        continue;
      }
      float a = (low - start) / direction, b = (high - start) / direction;
      const float sign = direction > 0 ? -1.0F : 1.0F;
      if (a > b)
        std::swap(a, b);
      if (a >= enter) {
        enter = a;
        normal = {};
        component(normal, axis, sign);
      }
      leave = std::min(leave, b);
      if (enter > leave) {
        valid = false;
        break;
      }
    }
    if (valid && enter <= nearest && leave >= 0) {
      nearest = enter;
      found = true;
      hit = {2,
             sizeof(hit),
             id,
             enter,
             {ray.start.x + (ray.end.x - ray.start.x) * enter,
              ray.start.y + (ray.end.y - ray.start.y) * enter,
              ray.start.z + (ray.end.z - ray.start.z) * enter},
             normal};
    }
  }
  return found;
}
void World::step(const std::string &owner, float seconds) {
  if (!std::isfinite(seconds) || seconds <= 0)
    return;
  seconds = std::min(seconds, 0.1F);
  for (auto &[id, a] : actors_)
    if (a.owner == owner) {
      auto &s = a.state;
      s.position.x = std::clamp(s.position.x + s.velocity.x * seconds,
                                -30000.0F, 30000.0F);
      s.position.y = std::clamp(s.position.y + s.velocity.y * seconds,
                                -30000.0F, 30000.0F);
      s.position.z = std::clamp(s.position.z + s.velocity.z * seconds,
                                -30000.0F, 30000.0F);
    }
}
std::vector<World::Draw> World::draws() const {
  std::vector<Draw> result;
  for (const auto &[id, a] : actors_)
    if (a.state.visible) {
      const auto m = meshes_.find(a.state.mesh);
      if (m != meshes_.end())
        result.push_back({id, a.state, &m->second.mesh, a.orientation});
    }
  return result;
}
} // namespace rocket::mods::sdk
