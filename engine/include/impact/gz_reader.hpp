#pragma once

#include <zlib.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Shared by tardis_main.cpp (validation replay) and bench_main.cpp (benchmark harness): both
// stream a Tardis gz file a megabyte at a time so a multi-gigabyte day never lands in memory.
// Extracted here so the two drivers read the identical bytes in the identical order; the
// benchmark's parse cost and the validation replay's parse cost are then the same code path,
// not two implementations that could quietly drift apart.

namespace impact {

/// Streams lines out of a gzip file without decompressing it to disk or to one big buffer.
class GzLineReader {
public:
    explicit GzLineReader(const std::string& path) : file_(gzopen(path.c_str(), "rb")) {
        if (file_ != nullptr) {
            gzbuffer(file_, 1u << 20);
        }
        buffer_.resize(1u << 20);
    }
    ~GzLineReader() {
        if (file_ != nullptr) {
            gzclose(file_);
        }
    }
    GzLineReader(const GzLineReader&) = delete;
    GzLineReader& operator=(const GzLineReader&) = delete;

    bool ok() const { return file_ != nullptr; }

    /// Points `line` at the next line, without its terminator. False at end of file.
    bool next(std::string_view& line) {
        while (true) {
            const std::size_t nl = pending_.find('\n', scan_);
            if (nl != std::string::npos) {
                std::size_t end = nl;
                if (end > cursor_ && pending_[end - 1] == '\r') {
                    --end;
                }
                line = std::string_view(pending_).substr(cursor_, end - cursor_);
                cursor_ = nl + 1;
                scan_ = cursor_;
                return true;
            }
            if (!refill()) {
                if (cursor_ >= pending_.size()) {
                    return false;
                }
                line = std::string_view(pending_).substr(cursor_);
                cursor_ = pending_.size();
                scan_ = cursor_;
                return !line.empty();
            }
        }
    }

    /// Bytes consumed from the compressed file, for a progress percentage.
    std::int64_t compressed_offset() const {
        return file_ == nullptr ? 0 : static_cast<std::int64_t>(gzoffset(file_));
    }

private:
    bool refill() {
        pending_.erase(0, cursor_);
        scan_ = pending_.size();
        cursor_ = 0;
        const int got = gzread(file_, buffer_.data(), static_cast<unsigned>(buffer_.size()));
        if (got <= 0) {
            return false;
        }
        pending_.append(buffer_.data(), static_cast<std::size_t>(got));
        return true;
    }

    gzFile file_ = nullptr;
    std::vector<char> buffer_;
    std::string pending_;
    std::size_t cursor_ = 0;  ///< start of the unread line
    std::size_t scan_ = 0;    ///< how far the newline search already reached
};

}  // namespace impact
