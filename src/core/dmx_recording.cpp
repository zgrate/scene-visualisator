#include "core/dmx_recording.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <miniz.h>
#include <stdexcept>

namespace sv {

namespace {
constexpr char magic[6] = {'S', 'V', 'D', 'M', 'X', 1};
constexpr std::size_t record_size = 4 + 2 + 512;
constexpr std::uint32_t block_ms = 1000;

void put32(std::vector<std::uint8_t> &v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i)
        v.push_back(std::uint8_t(x >> (8 * i)));
}
std::uint32_t get32(const std::uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | std::uint32_t(p[3]) << 24; }
} // namespace

DmxRecordWriter::DmxRecordWriter(const std::filesystem::path &path) : out_(path, std::ios::binary | std::ios::trunc) {
    if (!out_)
        throw std::runtime_error("Cannot create " + path.filename().generic_string());
    out_.write(magic, sizeof magic);
}

DmxRecordWriter::~DmxRecordWriter() { close(); }

void DmxRecordWriter::append(std::uint32_t t_ms, std::uint16_t universe, const DmxSlots &data) {
    put32(raw_, t_ms);
    raw_.push_back(std::uint8_t(universe)), raw_.push_back(std::uint8_t(universe >> 8));
    raw_.insert(raw_.end(), data.begin(), data.end());
}

void DmxRecordWriter::write(std::uint32_t t_ms, std::uint16_t universe, std::span<const std::uint8_t, 512> data) {
    if (!out_.is_open())
        return;
    if (open_block_ && t_ms >= block_start_ + block_ms)
        flush_block();
    if (!open_block_) {
        open_block_ = true, block_start_ = t_ms;
        for (const auto &[u, d] : last_) // keyframe
            append(t_ms, u, d);
    }
    auto [it, added] = last_.try_emplace(universe);
    if (!added && std::ranges::equal(it->second, data))
        return;
    std::ranges::copy(data, it->second.begin());
    append(t_ms, universe, it->second);
}

void DmxRecordWriter::flush_block() {
    if (!open_block_)
        return;
    open_block_ = false;
    mz_ulong size = mz_compressBound(mz_ulong(raw_.size()));
    std::vector<std::uint8_t> block;
    put32(block, block_start_), put32(block, std::uint32_t(raw_.size())), put32(block, 0);
    block.resize(12 + size);
    if (mz_compress2(block.data() + 12, &size, raw_.data(), mz_ulong(raw_.size()), 1) != MZ_OK) {
        error_ = true;
        return;
    }
    block.resize(12 + size);
    for (int i = 0; i < 4; ++i)
        block[8 + std::size_t(i)] = std::uint8_t(std::uint32_t(size) >> (8 * i));
    out_.write(reinterpret_cast<const char *>(block.data()), std::streamsize(block.size()));
    out_.flush();
    error_ = error_ || !out_;
    raw_.clear();
}

void DmxRecordWriter::close() {
    if (!out_.is_open())
        return;
    flush_block();
    out_.close();
}

DmxRecording DmxRecording::load(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Cannot open " + path.filename().generic_string());
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
    if (bytes.size() < sizeof magic || std::memcmp(bytes.data(), magic, sizeof magic) != 0)
        throw std::runtime_error(path.filename().generic_string() + " is not a show recording");
    DmxRecording r;
    for (std::size_t at = sizeof magic; at + 12 <= bytes.size();) {
        const std::uint32_t comp = get32(&bytes[at + 8]);
        if (at + 12 + comp > bytes.size())
            break; // cut mid-block by a crash
        r.blocks_.push_back(
            {get32(&bytes[at]),
             get32(&bytes[at + 4]),
             {bytes.begin() + std::ptrdiff_t(at + 12), bytes.begin() + std::ptrdiff_t(at + 12 + comp)}});
        at += 12 + comp;
    }
    if (!r.blocks_.empty()) {
        r.state_at(UINT32_MAX); // decompresses the last block
        if (r.raw_.size() >= record_size)
            r.duration_ = get32(&r.raw_[r.raw_.size() - record_size]);
    }
    return r;
}

std::map<std::uint16_t, DmxSlots> DmxRecording::state_at(std::uint32_t t_ms) const {
    std::map<std::uint16_t, DmxSlots> out;
    const auto it = std::ranges::upper_bound(blocks_, t_ms, {}, &Block::t_start);
    if (it == blocks_.begin())
        return out;
    const int index = int(it - blocks_.begin()) - 1;
    if (index != cached_) {
        const Block &b = blocks_[std::size_t(index)];
        raw_.resize(b.raw_size);
        mz_ulong size = b.raw_size;
        if (mz_uncompress(raw_.data(), &size, b.compressed.data(), mz_ulong(b.compressed.size())) != MZ_OK ||
            size != b.raw_size)
            throw std::runtime_error("Show recording is damaged");
        cached_ = index;
    }
    for (std::size_t at = 0; at + record_size <= raw_.size(); at += record_size) {
        if (get32(&raw_[at]) > t_ms)
            break;
        std::memcpy(out[std::uint16_t(raw_[at + 4] | raw_[at + 5] << 8)].data(), &raw_[at + 6], 512);
    }
    return out;
}

} // namespace sv
