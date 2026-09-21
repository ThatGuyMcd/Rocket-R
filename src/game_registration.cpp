#include "game_registration.hpp"

#include "rom_identity.generated.hpp"
#include "bootstrap.generated.hpp"
#include "librecomp/game.hpp"
#include "recomp.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" void recomp_entrypoint(std::uint8_t* rdram, recomp_context* context);
extern gpr get_entrypoint_address();

namespace {
std::atomic<bool> g_rom_ready{false};
std::atomic<bool> g_started{false};

constexpr gpr sign_extended_address(const std::uint32_t address) {
    return static_cast<gpr>(static_cast<std::int32_t>(address));
}

void InitialiseEntrypointContext(std::uint8_t* rdram, recomp_context* context) {
    // Rocket's retail 0x80000400 bootstrap clears codesegs0_1 BSS, sets
    // its caller stack to 0x803FFFF0, then calls game_init. gIdleThreadStack
    // is a different stack created later for thread 1 inside game_init.
    // N64ModernRuntime initially
    // zero-fills RDRAM, but its 1 MiB ROM copy reaches into Rocket's first BSS,
    // so reproduce the retail clear explicitly before entering game_init.
    const std::uint32_t bss_start = rocket::generated::kBootstrapBssStart & 0x3FFFFFFFU;
    const std::uint32_t bss_end = rocket::generated::kBootstrapBssEnd & 0x3FFFFFFFU;
    std::memset(rdram + bss_start, 0, static_cast<std::size_t>(bss_end - bss_start));
    context->r29 = sign_extended_address(rocket::generated::kInitialStackPointer);
    std::fprintf(stderr,
                 "[boot] bootstrap context: cleared BSS 0x%08X-0x%08X; sp=0x%08X; callable game_init=0x%08X; ROM load=0x%08X\n",
                 rocket::generated::kBootstrapBssStart,
                 rocket::generated::kBootstrapBssEnd,
                 rocket::generated::kInitialStackPointer,
                 rocket::generated::kCallableEntrypoint,
                 rocket::generated::kRetailLoadAddress);
}

void RunRocketEntrypoint(std::uint8_t* rdram, recomp_context* context) {
    // Do not execute Rocket's raw retail entry stub here. Under the static
    // runtime game_init legitimately returns after starting the idle thread;
    // the cartridge stub then executes its intentional BREAK at 0x80000438.
    // DKR-R uses the same split: retain 0x80000400 for ROM placement, but call
    // the post-bootstrap game startup through a host wrapper.
    recomp_entrypoint(rdram, context);
}
}

bool rocket::register_game(const std::filesystem::path& config_directory, std::string& error) {
    recomp::register_config_path(config_directory);
    register_generated_sections();

    const gpr generated_entrypoint = get_entrypoint_address();
    const std::uint32_t generated_address = static_cast<std::uint32_t>(generated_entrypoint);
    if (generated_address != generated::kCallableEntrypoint) {
        char buffer[320]{};
        std::snprintf(buffer, sizeof(buffer),
                      "Generated Rocket callable entrypoint is 0x%08X; expected game_init 0x%08X. Regenerate stage 7 with FIXED34.",
                      generated_address, generated::kCallableEntrypoint);
        error = buffer;
        return false;
    }
    if (generated::kRetailLoadAddress != kRetailEntrypoint) {
        error = "Generated Rocket retail load address drifted from 0x80000400.";
        return false;
    }

    recomp::GameEntry entry{};
    entry.rom_hash = generated::kRomXxh3;
    entry.internal_name = "ROCKETROBOTONWHEELS";
    entry.game_id = kGameId;
    entry.mod_game_id = "rocket";
    entry.save_type = recomp::SaveType::Eep4k;
    entry.is_enabled = true;
    entry.decompression_routine = nullptr;
    entry.has_compressed_code = false;

    // N64ModernRuntime uses entrypoint_address as the destination of its
    // initial 1 MiB ROM copy. Keep this at the cartridge's real load address.
    // The callable function is intentionally different: RunRocketEntrypoint
    // invokes N64Recomp's game_init payload after on_init_callback recreates
    // the bootstrap stack state.
    entry.entrypoint_address = sign_extended_address(generated::kRetailLoadAddress);
    entry.entrypoint = RunRocketEntrypoint;
    entry.thread_create_callback = nullptr;
    entry.on_init_callback = InitialiseEntrypointContext;

    if (!recomp::register_game(entry)) {
        error = "N64ModernRuntime rejected the Rocket game registration.";
        return false;
    }

    std::fprintf(stderr,
                 "[boot] registered Rocket: ROM DMA/load=0x%08X, callable game_init=0x%08X, bootstrap sp=0x%08X\n",
                 generated::kRetailLoadAddress,
                 generated::kCallableEntrypoint,
                 generated::kInitialStackPointer);
    error.clear();
    return true;
}

bool rocket::select_rom(const std::filesystem::path& rom_path, std::string& error) {
    std::u8string game_id{kGameId};
    const auto result = recomp::select_rom(rom_path, game_id);
    switch (result) {
        case recomp::RomValidationError::Good:
            error.clear();
            g_rom_ready.store(true, std::memory_order_release);
            return true;
        case recomp::RomValidationError::FailedToOpen:
            error = "Could not open the selected ROM."; break;
        case recomp::RomValidationError::NotARom:
            error = "The selected file is not a recognised N64 ROM."; break;
        case recomp::RomValidationError::IncorrectVersion:
            error = "This is Rocket: Robot on Wheels, but it is not the supported US NSUE dump."; break;
        case recomp::RomValidationError::IncorrectRom:
            error = "The selected ROM is not the supported Rocket: Robot on Wheels US release."; break;
        default:
            error = "N64ModernRuntime rejected the selected ROM."; break;
    }
    return false;
}

bool rocket::rom_ready() {
    return g_rom_ready.load(std::memory_order_acquire);
}

bool rocket::start_game_once() {
    if (!g_rom_ready.load(std::memory_order_acquire)) {
        return false;
    }
    if (g_started.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    std::fprintf(stderr,
                 "[boot] RT64 received its first safe VI update; starting Rocket via game_init 0x%08X (ROM remains loaded at 0x%08X)\n",
                 generated::kCallableEntrypoint,
                 generated::kRetailLoadAddress);
    std::u8string game_id{kGameId};
    recomp::start_game(game_id);
    return true;
}
