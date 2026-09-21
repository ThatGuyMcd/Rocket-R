#pragma once

#include <filesystem>

namespace rocket::diagnostics {
void install(const std::filesystem::path& config_directory);
}
