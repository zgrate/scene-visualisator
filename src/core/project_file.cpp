#include "core/project_file.h"

#include <fstream>
#include <miniz.h>

namespace fs = std::filesystem;

namespace sv {

namespace {

constexpr std::uint64_t max_entry_size = 1ull << 30; // refuse zip bombs and absurd entries

std::vector<std::uint8_t> read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw ProjectFileError("cannot open " + path.string());
    return {std::istreambuf_iterator<char>(in), {}};
}

std::vector<std::uint8_t> to_zip(const Package &pkg) {
    ZipEntries entries = pkg.files;
    const std::string doc = pkg.project.dump(2) + "\n";
    entries["project.json"].assign(doc.begin(), doc.end());
    return write_zip(entries);
}

void write_atomic(const fs::path &path, const std::vector<std::uint8_t> &bytes, bool keep_backups) {
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
        out.close();
        // shortcut: no fsync, so a power cut (not a crash) can still lose the last save; add platform fsync for Beta.
        if (!out)
            throw ProjectFileError("cannot write " + tmp.string());
    }
    if (keep_backups && fs::exists(path)) {
        const auto bak = [&](int n) { return fs::path(path.string() + ".bak" + std::to_string(n)); };
        fs::remove(bak(backup_count));
        for (int n = backup_count - 1; n >= 1; --n)
            if (fs::exists(bak(n)))
                fs::rename(bak(n), bak(n + 1));
        fs::copy_file(path, bak(1));
    }
    fs::rename(tmp, path); // replaces the old file in one step on POSIX and Windows
}

} // namespace

ZipEntries read_zip(std::span<const std::uint8_t> bytes) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0))
        throw ProjectFileError("not a zip file");
    ZipEntries out;
    std::string error;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip) && error.empty(); ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) {
            error = "corrupt zip directory";
            break;
        }
        if (st.m_is_directory)
            continue;
        if (st.m_uncomp_size > max_entry_size) {
            error = std::string("entry too large: ") + st.m_filename;
            break;
        }
        std::size_t size = 0;
        void *data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!data) {
            error = std::string("cannot extract ") + st.m_filename;
            break;
        }
        const auto *p = static_cast<const std::uint8_t *>(data);
        out[st.m_filename].assign(p, p + size);
        mz_free(data);
    }
    mz_zip_reader_end(&zip);
    if (!error.empty())
        throw ProjectFileError(error);
    return out;
}

std::vector<std::uint8_t> write_zip(const ZipEntries &entries) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0))
        throw ProjectFileError("zip writer init failed");
    bool ok = true;
    for (const auto &[name, bytes] : entries)
        ok = ok && mz_zip_writer_add_mem(&zip, name.c_str(), bytes.data(), bytes.size(), MZ_DEFAULT_COMPRESSION);
    void *buf = nullptr;
    std::size_t size = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &buf, &size);
    std::vector<std::uint8_t> out;
    if (ok)
        out.assign(static_cast<std::uint8_t *>(buf), static_cast<std::uint8_t *>(buf) + size);
    mz_free(buf);
    mz_zip_writer_end(&zip);
    if (!ok)
        throw ProjectFileError("zip write failed");
    return out;
}

Package load_package(const fs::path &path) {
    Package pkg;
    bool have_doc = false;
    std::string error;
    try {
        pkg.files = read_zip(read_file(path));
    } catch (const ProjectFileError &e) {
        throw ProjectFileError(path.string() + " is not a valid .svproj package (" + e.what() + ")");
    }
    if (auto it = pkg.files.find("project.json"); it != pkg.files.end()) {
        pkg.project = json::parse(it->second.begin(), it->second.end(), nullptr, false);
        have_doc = !pkg.project.is_discarded();
        if (!have_doc)
            error = "project.json is not valid JSON";
        pkg.files.erase(it);
    }

    if (error.empty() && !have_doc)
        error = "project.json missing";
    if (error.empty()) {
        const json &v = pkg.project.contains("schemaVersion") ? pkg.project["schemaVersion"] : json();
        if (!v.is_number_integer())
            error = "schemaVersion missing";
        else if (v.get<int>() > schema_version)
            error = "made by a newer version (schema " + std::to_string(v.get<int>()) + "), please update";
        else if (!pkg.project.contains("objects") || !pkg.project["objects"].is_array())
            error = "objects missing";
        else
            migrate(pkg.project);
    }
    if (!error.empty())
        throw ProjectFileError(path.string() + ": " + error);
    return pkg;
}

void save_package(const fs::path &path, const Package &pkg) {
    write_atomic(path, to_zip(pkg), true);
    fs::remove(autosave_path(path));
}

fs::path autosave_path(const fs::path &path) { return fs::path(path.string() + ".autosave"); }

void write_autosave(const fs::path &path, const Package &pkg) { write_atomic(autosave_path(path), to_zip(pkg), false); }

bool has_newer_autosave(const fs::path &path) {
    const fs::path a = autosave_path(path);
    std::error_code ec;
    if (!fs::exists(a, ec))
        return false;
    return !fs::exists(path, ec) || fs::last_write_time(a, ec) >= fs::last_write_time(path, ec);
}

} // namespace sv
