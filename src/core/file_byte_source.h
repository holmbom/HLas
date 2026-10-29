#pragma once
#include "byte_source.h"
#include "file_io.h"
#include <cstring>
#include <memory>
#include <string>

namespace HLas::detail {

// ByteSource över en mmap:ad fil. Läsningen är en memcpy ur sidcachen.
//
// Kopian är medvetet accepterad: den här stigen används för komprimerade
// chunkar som ändå ska avkodas, och memcpy:n är försumbar bredvid den
// aritmetiska avkodningen. De heta punktstigarna går inte hit — de läser
// nollkopierat genom PointSource.
class FileByteSource final : public ByteSource {
public:
    FileByteSource() = default;

    ErrorCode open(const char* path) {
        path_ = path ? path : "";
        return file_.open(path);
    }
    void close() { file_.close(); path_.clear(); }
    bool is_open() const { return file_.is_open(); }

    // Underliggande buffert, för kod som vill läsa nollkopierat när källan
    // råkar vara en fil. Är nullptr för källor som inte kan det.
    const uint8_t* data() const { return file_.data(); }

    bool read(uint64_t offset, size_t len, uint8_t* dest) override {
        if (!file_.is_open() || !dest)
            return false;
        if (!range_ok(offset, len, file_.size()))
            return false;
        if (len)
            std::memcpy(dest, file_.data() + offset, len);
        return true;
    }

    uint64_t size() const override { return file_.size(); }

    // En mmap:ad fil hade i och för sig kunnat delas — läsningen är en memcpy
    // ur en skrivskyddad mappning. Men att klona är billigt (mappningen är lat)
    // och håller regeln enkel: varje tråd äger sin källa.
    std::unique_ptr<ByteSource> clone() const override {
        if (path_.empty()) return nullptr;
        auto c = std::make_unique<FileByteSource>();
        if (c->open(path_.c_str()) != ErrorCode::Ok) return nullptr;
        return c;
    }

private:
    MappedFile  file_;
    std::string path_;
};

} // namespace HLas::detail
