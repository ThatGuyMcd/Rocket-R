#pragma once
#include "rocket/sdk_types.h"
#include <cstdint>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace rocket::mods::sdk {
// PCM runs through the existing output blocks. It never drives guest AI timing.
class Audio {
public:
  std::uint32_t load(const std::string &owner,
                     std::span<const std::uint8_t> wav);
  std::uint32_t play(const std::string &owner, const RocketAudioPlay &play);
  bool stop(const std::string &owner, std::uint32_t voice);
  bool playing(const std::string &owner, std::uint32_t voice) const;
  void clear(const std::string &owner);
  void reset();
  void pause(bool value);
  void mix(std::span<std::int16_t> stereo, unsigned rate, float master_gain);

private:
  struct Clip {
    std::string owner;
    unsigned rate;
    std::vector<std::int16_t> stereo;
  };
  struct Voice {
    std::string owner;
    std::uint32_t clip;
    double frame;
    bool loop;
    float left, right;
  };
  std::uint32_t handle();
  mutable std::mutex mutex_;
  std::map<std::uint32_t, Clip> clips_;
  std::map<std::uint32_t, Voice> voices_;
  std::uint32_t next_ = 1;
  bool paused_ = true;
};
Audio &audio();
} // namespace rocket::mods::sdk
