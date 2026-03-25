#include "nix/expr/trace-file.hh"
#include "nix/util/error.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/users.hh"

#include <nlohmann/json.hpp>

namespace nix {

TraceFile::TraceFile(std::filesystem::path path, std::function<void()> onClose)
    : path(std::move(path))
    , file(this->path, std::ios::out | std::ios::trunc)
    , onClose(std::move(onClose))
{
    if (!file.is_open())
        throw Error("failed to open trace file: %s", this->path.string());
    file << "[\n";
}

TraceFile::~TraceFile()
{
    if (file.is_open()) {
        file << "\n]\n";
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

    auto target = std::filesystem::read_symlink(latestLink);
    if (!std::filesystem::exists(target))
        return std::nullopt;

    return target;
}

std::vector<trace::TraceEntry> TracingDatabase::parseTraceFile(const std::filesystem::path & tracePath) const
{
    std::vector<trace::TraceEntry> entries;

    std::ifstream file(tracePath);
    if (!file.is_open())
        throw Error("could not open trace file: %s", tracePath.string());

    auto json = nlohmann::json::parse(file);
    for (const auto & j : json) {
        if (auto entry = trace::parseTraceEntry(j))
            entries.push_back(std::move(*entry));
    }

    return entries;
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
            if (entry.contains("request") && entry["request"].contains("absPath")) {
                auto path = entry["request"]["absPath"].get<std::string>();
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
