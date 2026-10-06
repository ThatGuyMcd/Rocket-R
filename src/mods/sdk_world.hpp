#pragma once
#include "mod_library.hpp"
#include "rocket/sdk_types.h"
#include <array>
#include <functional>

namespace rocket::mods::sdk {
struct MeshVertex {
  RocketVec3 position;
  std::uint32_t rgba;
  float u = 0, v = 0;
};
struct Mesh {
  std::vector<MeshVertex> vertices;
  std::vector<std::array<std::uint32_t, 3>> triangles;
  unsigned width = 0, height = 0;
  std::vector<std::uint16_t> texture;
};
Mesh decode_mesh(std::span<const std::uint8_t> bytes);
class World {
public:
  using Loader = std::function<std::vector<std::uint8_t>(const std::string &)>;
  struct Draw {
    std::uint32_t handle;
    RocketActorState state;
    const Mesh *mesh;
    std::array<float, 4> orientation;
  };
  void reset();
  void clear(const std::string &owner);
  std::uint32_t mesh(const std::string &owner,
                     std::span<const std::uint8_t> data);
  std::uint32_t create(const std::string &owner, const RocketActorState &state);
  bool read(const std::string &owner, std::uint32_t handle,
            RocketActorState &state) const;
  bool update(const std::string &owner, std::uint32_t handle,
              const RocketActorState &state);
  bool remove(const std::string &owner, std::uint32_t handle);
  bool pose(const std::string &owner, const RocketActorPose &pose);
  std::uint32_t find(const std::string &owner, const std::string &name) const;
  void scene(const std::string &owner, const Json &scene, const Loader &loader,
             const RocketVec3 &origin = {});
  bool ray(const std::string &owner, const RocketRay &ray,
           RocketHit &hit) const;
  void move(const std::string &owner, RocketMotion &motion) const;
  void step(const std::string &owner, float seconds);
  std::vector<Draw> draws() const;

private:
  struct OwnedMesh {
    std::string owner;
    Mesh mesh;
  };
  struct Actor {
    std::string owner;
    RocketActorState state;
    std::string name;
    std::array<float, 4> orientation{0, 0, 0, 1};
  };
  struct Box {
    std::string owner;
    RocketVec3 low, high;
  };
  void validate(const std::string &owner, const RocketActorState &state) const;
  std::uint32_t handle();
  std::uint32_t next_ = 1;
  std::map<std::uint32_t, OwnedMesh> meshes_;
  std::map<std::uint32_t, Actor> actors_;
  std::map<std::uint32_t, Box> boxes_;
};
} // namespace rocket::mods::sdk
