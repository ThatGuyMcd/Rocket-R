#pragma once

#include <cstdint>

namespace rocket::renderer {

class SnapshotScope {
public:
    SnapshotScope(std::uint8_t*& core_rdram,
                  std::uint8_t*& state_rdram,
                  std::uint8_t* submission_rdram);
    ~SnapshotScope();

    SnapshotScope(const SnapshotScope&) = delete;
    SnapshotScope& operator=(const SnapshotScope&) = delete;

private:
    std::uint8_t*& core_rdram_;
    std::uint8_t*& state_rdram_;
    std::uint8_t* original_core_ = nullptr;
    std::uint8_t* original_state_ = nullptr;
};

} // namespace rocket::renderer
