#pragma once
#include <filesystem>
namespace rocket::mods::ui {
void draw(bool expand_details = false);
void import_file(const std::filesystem::path& path);
void launch_summary();
bool prepare_launch();
}
