// SPDX-License-Identifier: GPL-2.0-or-later
// Port shim: minimal stdio-backed file used by the optional shader dump paths.

#pragma once

#include <cstdio>
#include <filesystem>
#include <string_view>

namespace Common::FS {

enum class FileAccessMode { Read, Write, Append, ReadWrite, Create };
enum class FileType { BinaryFile, TextFile };

class IOFile {
public:
    IOFile(const std::filesystem::path& path, FileAccessMode mode,
           FileType type = FileType::BinaryFile) {
        const char* open_mode = "wb";
        if (type == FileType::TextFile) {
            open_mode = mode == FileAccessMode::Append ? "a" : "w";
        }
        file_ = std::fopen(path.c_str(), open_mode);
    }

    ~IOFile() {
        if (file_) {
            std::fclose(file_);
        }
    }

    IOFile(const IOFile&) = delete;
    IOFile& operator=(const IOFile&) = delete;

    void WriteString(std::string_view text) const {
        WriteRaw<char>(text.data(), text.size());
    }

    // Writes `count` elements of T starting at `data`; the caller's pointer type does not matter.
    template <typename T>
    void WriteRaw(const void* data, std::size_t count) const {
        if (file_) {
            std::fwrite(data, sizeof(T), count, file_);
        }
    }

private:
    std::FILE* file_ = nullptr;
};

} // namespace Common::FS
