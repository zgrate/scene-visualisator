#pragma once
// Show recording: the merged DMX of every universe over time (.svdmx). The picture goes to an .mp4 beside it.
// File: "SVDMX\1", then blocks of {u32 t_start_ms, u32 raw_size, u32 comp_size, deflate data}. A block's raw data
// is records of {u32 t_ms, u16 universe, 512 bytes}; it opens with a keyframe of every universe seen so far, so a
// seek reads one block.
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace sv {

using DmxSlots = std::array<std::uint8_t, 512>;

class DmxRecordWriter {
  public:
    explicit DmxRecordWriter(const std::filesystem::path &path); // throws std::runtime_error if the file cannot be created
    ~DmxRecordWriter();
    DmxRecordWriter(const DmxRecordWriter &) = delete;
    DmxRecordWriter &operator=(const DmxRecordWriter &) = delete;

    // Times must not go backwards. Unchanged universes are skipped.
    void write(std::uint32_t t_ms, std::uint16_t universe, std::span<const std::uint8_t, 512> data);
    void close();
    bool error() const { return error_; }

  private:
    void flush_block();
    void append(std::uint32_t t_ms, std::uint16_t universe, const DmxSlots &data);

    std::ofstream out_;
    std::map<std::uint16_t, DmxSlots> last_;
    std::vector<std::uint8_t> raw_;
    std::uint32_t block_start_ = 0;
    bool open_block_ = false, error_ = false;
};

class DmxRecording {
  public:
    // Throws std::runtime_error on a missing file or bad magic. A tail cut mid-block (a crash) is ignored.
    static DmxRecording load(const std::filesystem::path &path);

    std::uint32_t duration_ms() const { return duration_; }
    std::map<std::uint16_t, DmxSlots> state_at(std::uint32_t t_ms) const;

  private:
    struct Block {
        std::uint32_t t_start, raw_size;
        std::vector<std::uint8_t> compressed;
    };
    std::vector<Block> blocks_;
    std::uint32_t duration_ = 0;
    mutable int cached_ = -1;
    mutable std::vector<std::uint8_t> raw_;
};

} // namespace sv
