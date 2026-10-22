#pragma once
// FR-1: .svproj package = zip of project.json plus gdtf/, geometry/, media/, exports/ entries.
#include "core/model.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace sv {

using ZipEntries = std::map<std::string, std::vector<std::uint8_t>>;

struct Package {
    json project = new_project();
    // Every other entry, by path inside the zip (e.g. "gdtf/Generic@Spot@1.gdtf").
    // shortcut: assets are held in memory, stream media files from disk once video sources land (Beta).
    ZipEntries files;
};

struct ProjectFileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline constexpr int backup_count = 3;

// Plain zip helpers, shared with MVR (a zip too). read_zip throws ProjectFileError for corrupt or oversized entries.
ZipEntries read_zip(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> write_zip(const ZipEntries &entries);

// Throws ProjectFileError for unreadable, corrupt, or newer-schema packages.
Package load_package(const std::filesystem::path &path);

// Atomic: writes <path>.tmp, rotates <path>.bak1..3, then renames over <path>. Removes a stale autosave.
void save_package(const std::filesystem::path &path, const Package &pkg);

// Autosave (every 2 minutes, driven by the app) goes to <path>.autosave, also atomically.
std::filesystem::path autosave_path(const std::filesystem::path &path);
void write_autosave(const std::filesystem::path &path, const Package &pkg);
// AT-6: true when an autosave exists that is newer than the saved package (offer recovery on launch).
bool has_newer_autosave(const std::filesystem::path &path);

} // namespace sv
