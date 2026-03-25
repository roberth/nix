#include "nix/expr/trace-file.hh"
#include "nix/util/error.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/users.hh"

#include <nlohmann/json.hpp>

namespace nix {

TraceFile::TraceFile(std::filesystem::path path)
    : path(std::move(path))
    , file(this->path, std::ios::out | std::ios::trunc)
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
    auto tracePath = tracesDir() / (hash.to_string(HashFormat::Nix32, false) + ".json");

    // Update symlink to point to the latest trace
    auto latestLink = basePath / "latest.json";
    std::filesystem::remove(latestLink);
    std::filesystem::create_symlink(tracePath, latestLink);

    return tracePath;
}

} // namespace nix
