#include "sdk_audio.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace rocket::mods::sdk {
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t p) {
  return b[p] | (std::uint32_t(b[p + 1]) << 8) |
         (std::uint32_t(b[p + 2]) << 16) | (std::uint32_t(b[p + 3]) << 24);
}
std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t p) {
  return b[p] | (std::uint16_t(b[p + 1]) << 8);
}
} // namespace
Audio &audio() {
  static Audio value;
  return value;
}
std::uint32_t Audio::handle() {
  require(next_ != 0, "Audio handles exhausted.");
  return next_++;
}
std::uint32_t Audio::load(const std::string &owner,
                          std::span<const std::uint8_t> wav) {
  require(wav.size() >= 12 && wav.size() <= 16 * 1024 * 1024 &&
              std::memcmp(wav.data(), "RIFF", 4) == 0 &&
              std::memcmp(wav.data() + 8, "WAVE", 4) == 0,
          "Audio needs a RIFF/WAVE file.");
  require(u32(wav, 4) == wav.size() - 8, "Invalid WAV container size.");
  unsigned channels = 0, rate = 0;
  std::size_t start = 0, length = 0;
  bool format = false, data = false;
  for (std::size_t p = 12; p < wav.size();) {
    require(wav.size() - p >= 8, "Truncated WAV chunk.");
    const auto size = u32(wav, p + 4);
    require(size <= wav.size() - p - 8, "WAV chunk exceeds its container.");
    if (std::memcmp(wav.data() + p, "fmt ", 4) == 0) {
      require(!format && size >= 16 && u16(wav, p + 8) == 1,
              "WAV needs uncompressed PCM.");
      channels = u16(wav, p + 10);
      rate = u32(wav, p + 12);
      require((channels == 1 || channels == 2) && rate >= 8000 &&
                  rate <= 96000 && u16(wav, p + 22) == 16 &&
                  u16(wav, p + 20) == channels * 2 &&
                  u32(wav, p + 16) == rate * channels * 2,
              "WAV needs 16-bit mono/stereo PCM at 8–96 kHz.");
      format = true;
    }
    if (std::memcmp(wav.data() + p, "data", 4) == 0) {
      require(!data, "Duplicate WAV data chunk.");
      start = p + 8;
      length = size;
      data = true;
    }
    p += 8 + size + (size & 1);
    require(p <= wav.size(), "Missing WAV chunk padding.");
  }
  require(format && data && length > 0 && length % (channels * 2) == 0,
          "WAV has no complete PCM frames.");
  const auto frames = length / (channels * 2);
  std::lock_guard lock(mutex_);
  std::size_t resident = frames * 4;
  for (const auto &[id, clip] : clips_)
    resident += clip.stereo.size() * 2;
  require(clips_.size() < 128 && resident <= 32 * 1024 * 1024,
          "Custom audio exceeds its clip/memory budget.");
  Clip clip{owner, rate, {}};
  clip.stereo.reserve(frames * 2);
  for (std::size_t i = 0; i < frames; ++i) {
    const auto p = start + i * channels * 2;
    const auto left = static_cast<std::int16_t>(u16(wav, p));
    const auto right =
        channels == 2 ? static_cast<std::int16_t>(u16(wav, p + 2)) : left;
    clip.stereo.push_back(left);
    clip.stereo.push_back(right);
  }
  const auto id = handle();
  clips_.emplace(id, std::move(clip));
  return id;
}
std::uint32_t Audio::play(const std::string &owner, const RocketAudioPlay &p) {
  std::lock_guard lock(mutex_);
  const auto clip = clips_.find(p.clip);
  require(p.api == 2 && p.size >= sizeof(p) && clip != clips_.end() &&
              clip->second.owner == owner && p.loop <= 1 &&
              std::isfinite(p.volume) && p.volume >= 0 && p.volume <= 1 &&
              std::isfinite(p.pan) && p.pan >= -1 && p.pan <= 1,
          "Invalid audio play request or clip ownership.");
  require(voices_.size() < 32, "Custom audio exceeds 32 simultaneous voices.");
  const auto id = handle();
  voices_.emplace(id, Voice{owner, p.clip, 0, p.loop != 0,
                            p.volume * (p.pan > 0 ? 1 - p.pan : 1),
                            p.volume * (p.pan < 0 ? 1 + p.pan : 1)});
  return id;
}
bool Audio::stop(const std::string &owner, std::uint32_t voice) {
  std::lock_guard lock(mutex_);
  const auto it = voices_.find(voice);
  if (it == voices_.end() || it->second.owner != owner)
    return false;
  voices_.erase(it);
  return true;
}
bool Audio::playing(const std::string &owner, std::uint32_t voice) const {
  std::lock_guard lock(mutex_);
  const auto it = voices_.find(voice);
  return it != voices_.end() && it->second.owner == owner;
}
void Audio::clear(const std::string &owner) {
  std::lock_guard lock(mutex_);
  for (auto it = voices_.begin(); it != voices_.end();)
    if (it->second.owner == owner)
      it = voices_.erase(it);
    else
      ++it;
  for (auto it = clips_.begin(); it != clips_.end();)
    if (it->second.owner == owner)
      it = clips_.erase(it);
    else
      ++it;
}
void Audio::reset() {
  std::lock_guard lock(mutex_);
  voices_.clear();
  clips_.clear();
  paused_ = true;
}
void Audio::pause(bool value) {
  std::lock_guard lock(mutex_);
  paused_ = value;
}
void Audio::mix(std::span<std::int16_t> stereo, unsigned rate, float gain) {
  if (rate == 0 || stereo.size() % 2 || !std::isfinite(gain))
    return;
  std::lock_guard lock(mutex_);
  if (paused_ || voices_.empty())
    return;
  gain = std::clamp(gain, 0.0F, 1.0F);
  for (std::size_t i = 0; i < stereo.size(); i += 2) {
    float left = stereo[i], right = stereo[i + 1];
    for (auto it = voices_.begin(); it != voices_.end();) {
      auto &v = it->second;
      const auto &clip = clips_.at(v.clip);
      const auto frames = clip.stereo.size() / 2;
      if (v.frame >= frames) {
        if (!v.loop) {
          it = voices_.erase(it);
          continue;
        }
        v.frame = std::fmod(v.frame, static_cast<double>(frames));
      }
      const auto a = static_cast<std::size_t>(v.frame), b = a + 1 < frames
                                                                ? a + 1
                                                            : v.loop ? 0
                                                                     : a;
      const auto t = static_cast<float>(v.frame - a);
      left +=
          (clip.stereo[a * 2] + (clip.stereo[b * 2] - clip.stereo[a * 2]) * t) *
          v.left * gain;
      right += (clip.stereo[a * 2 + 1] +
                (clip.stereo[b * 2 + 1] - clip.stereo[a * 2 + 1]) * t) *
               v.right * gain;
      v.frame += static_cast<double>(clip.rate) / rate;
      ++it;
    }
    stereo[i] = static_cast<std::int16_t>(
        std::clamp(std::lround(left), -32768L, 32767L));
    stereo[i + 1] = static_cast<std::int16_t>(
        std::clamp(std::lround(right), -32768L, 32767L));
  }
}
} // namespace rocket::mods::sdk
