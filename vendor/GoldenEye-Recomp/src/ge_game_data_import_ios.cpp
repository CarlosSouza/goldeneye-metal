/**
 * @file        src/ge_game_data_import_ios.cpp
 * @brief       On-device game-data import hooks for the iPad app
 *
 * The shared iOS entry point (windowed_app_main_ios.mm) declares weak no-op
 * fallbacks for these hooks. Defining them here lets the first-run setup
 * screen recognize a legally owned LIVE/STFS backup dropped into Documents
 * and extract it into the Game Data tree on the iPad itself, with no Mac in
 * the loop. All validation, staging and atomic publishing is the same
 * ge::game_data::ImportPackage path the macOS launcher uses.
 */

#include "ge_game_data.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

extern "C" {

typedef void (*RexGameDataImportProgressFn)(void* ctx, const char* message,
                                            unsigned long long completed,
                                            unsigned long long total);
typedef bool (*RexGameDataImportCancelledFn)(void* ctx);

bool RexGameDataIsPackage(const char* path) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  // The retail package is ~700 MB; the floor skips stray files cheaply
  // before touching their contents.
  if (ec || size < 1024ull * 1024ull) {
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  char magic[4] = {};
  if (!file.read(magic, sizeof(magic))) {
    return false;
  }
  return std::memcmp(magic, "LIVE", 4) == 0 || std::memcmp(magic, "PIRS", 4) == 0 ||
         std::memcmp(magic, "CON ", 4) == 0;
}

char* RexGameDataImportPackage(const char* package_path, const char* destination,
                               RexGameDataImportProgressFn progress,
                               RexGameDataImportCancelledFn cancelled, void* ctx) {
  auto result = ge::game_data::ImportPackage(
      package_path, destination,
      [&](const ge::game_data::Progress& update) {
        if (progress) {
          progress(ctx, update.message.c_str(), update.completed, update.total);
        }
      },
      [&]() { return cancelled && cancelled(ctx); });
  if (result) {
    return nullptr;
  }
  return strdup(result.error.empty() ? "The import failed." : result.error.c_str());
}

}  // extern "C"
