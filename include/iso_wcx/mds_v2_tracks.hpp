#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace iso_wcx {
// Read-only bounded random access to a supported MDS v2 / monolithic MDX
// filesystem-bearing track. Metadata is decrypted in process; stored or
// raw-DEFLATE compression groups are decoded on demand, never via temporary
// files, helper processes or extra runtime DLLs.
// Data-sector AES-LRW and password-protected tracks intentionally fail closed.
class MdsV2TrackStream final {
public:
    explicit MdsV2TrackStream(const std::filesystem::path& descriptor);
    ~MdsV2TrackStream();
    MdsV2TrackStream(const MdsV2TrackStream&) = delete;
    MdsV2TrackStream& operator=(const MdsV2TrackStream&) = delete;
    [[nodiscard]] std::uint64_t size() const noexcept;
    [[nodiscard]] const std::string& label() const noexcept;
    [[nodiscard]] std::vector<std::uint8_t> read(std::uint64_t offset,
                                                 std::size_t length);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace iso_wcx
