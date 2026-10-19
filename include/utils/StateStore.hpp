#pragma once
#include <nlohmann/json.hpp>
#include <core/Logger.hpp>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>

namespace bot::state {

// Writes JSON so that a crash at any point leaves either the old or the new file, never a
// torn one: write to a temp file, flush it to disk, then atomically rename over the target.
inline bool writeJsonAtomic(const std::filesystem::path& path, const nlohmann::json& j) {
    namespace fs = std::filesystem;
    try {
        if (path.has_parent_path()) fs::create_directories(path.parent_path());
        fs::path tmp = path;
        tmp += ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc);
            out << j.dump(2);
            out.flush();
            if (!out) throw std::runtime_error("write failed");
        }
        // fsync the data before the rename makes it visible.
        if (FILE* f = std::fopen(tmp.c_str(), "r")) { ::fsync(fileno(f)); std::fclose(f); }
        fs::rename(tmp, path);
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("[StateStore] Failed to write {}: {}", path.string(), e.what());
        return false;
    }
}

// nullopt if the file doesn't exist. A corrupt file is an error the caller must not ignore.
inline std::optional<nlohmann::json> readJson(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return std::nullopt;
    std::ifstream in(path);
    return nlohmann::json::parse(in);  // throws on corruption, by design
}

} // namespace bot::state
