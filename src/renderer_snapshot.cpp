#include "renderer_snapshot.hpp"

rocket::renderer::SnapshotScope::SnapshotScope(
    std::uint8_t*& core_rdram, std::uint8_t*& state_rdram,
    std::uint8_t* submission_rdram)
    : core_rdram_(core_rdram), state_rdram_(state_rdram),
      original_core_(core_rdram), original_state_(state_rdram) {
    core_rdram_ = submission_rdram;
    state_rdram_ = submission_rdram;
}

rocket::renderer::SnapshotScope::~SnapshotScope() {
    state_rdram_ = original_state_;
    core_rdram_ = original_core_;
}
