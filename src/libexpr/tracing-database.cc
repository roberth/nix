#include "nix/expr/tracing-database.hh"
#include "nix/util/error.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/users.hh"

#include <iostream>
#include <nlohmann/json.hpp>

namespace nix {

TraceFile::TraceFile(std::filesystem::path path, std::function<void()> onClose)
    : path(path)
    , file(path, std::ios::out | std::ios::trunc)
    , onClose(std::move(onClose))
{
    if (!file.is_open())
        throw Error("failed to open trace file: %s", path.string());
    file << "[\n";
}

TraceFile::~TraceFile()
{
    if (file.is_open()) {
        file << "]\n";
        file.close();
        if (onClose)
            onClose();
    }
}

void TraceFile::log(const nlohmann::json & entry)
{
    if (!first)
        file << ",\n";
    first = false;
    file << entry.dump(2);
    file.flush();
}

uint64_t TraceFile::allocValue()
{
    return nextValueNum++;
}

TracingDatabase::TracingDatabase()
    : basePath(std::filesystem::path(getCacheDir()) / "eval-tracing-v0")
{
    createDirs(tracesDir());
}

std::filesystem::path TracingDatabase::tracesDir() const
{
    return basePath / "traces";
}

std::filesystem::path TracingDatabase::newTraceFile()
{
    auto hash = Hash::random(HashAlgorithm::SHA256);
    return tracesDir() / (hash.to_string(HashFormat::Nix32, false) + ".json");
}

void TracingDatabase::updateLatestSymlink(const std::filesystem::path & tracePath)
{
    auto latestLink = basePath / "latest.json";
    std::filesystem::remove(latestLink);
    std::filesystem::create_symlink(tracePath, latestLink);
}

std::optional<std::filesystem::path> TracingDatabase::latestTraceFile() const
{
    auto latestLink = basePath / "latest.json";
    if (!std::filesystem::exists(latestLink))
        return std::nullopt;
    return std::filesystem::read_symlink(latestLink);
}

std::vector<std::string> TracingDatabase::getTracedFilePaths(const std::filesystem::path & tracePath) const
{
    std::vector<std::string> paths;

    std::ifstream file(tracePath);
    if (!file.is_open())
        return paths;

    try {
        auto json = nlohmann::json::parse(file);
        for (const auto & entry : json) {
            // Look for FileReadRequest entries (they have request.absPath)
            if (entry.contains("request") && entry["request"].contains("absPath")) {
                auto path = entry["request"]["absPath"].get<std::string>();
                // Only include .nix files (skip shell scripts, patches, etc.)
                if (path.ends_with(".nix"))
                    paths.push_back(std::move(path));
            }
        }
    } catch (...) {
        // Ignore parse errors
    }

    return paths;
}

} // namespace nix
