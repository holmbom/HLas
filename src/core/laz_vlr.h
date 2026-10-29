#pragma once
#ifdef HLAS_HAVE_LAZ

#include "vlr_parser.h"
#include "las_error.h"
#include <cstdint>
#include <cstring>
#include <vector>

namespace HLas::detail {

enum class LazCompressor : uint16_t {
    None        = 0,
    Pointwise   = 1,  // single chunk, format 0-5
    Chunked     = 2,  // chunked, format 0-5
    LayerChunked = 3, // layered + chunked, format 6-10
};

enum class LazItemType : uint16_t {
    Byte          = 0,
    Point10       = 6,
    GPSTime11     = 7,
    RGB12         = 8,
    Wavepacket13  = 9,
    Point14       = 10,
    RGB14         = 11,
    RGBNIR14      = 12,
    Wavepacket14  = 13,
    Byte14        = 14,
};

struct LazItem {
    LazItemType type;
    uint16_t    size;
    uint16_t    version;
};

struct LazVlrData {
    LazCompressor compressor;
    uint16_t      coder;          // 0 = arithmetic
    uint8_t       version_major;
    uint8_t       version_minor;
    uint16_t      version_revision;
    uint32_t      options;
    uint32_t      chunk_size;
    int64_t       num_special_evlrs;
    int64_t       offset_special_evlrs;
    std::vector<LazItem> items;
};

inline ErrorCode parse_laz_vlr(const std::vector<Vlr>& vlrs,
                               LazVlrData& out) {
    const Vlr* v = find_vlr(vlrs, "laszip encoded", 22204);
    if (!v)
        return ErrorCode::LazNotSupported;

    const uint8_t* d = v->data;
    uint64_t sz = v->data_size;

    // Minimum: 34 bytes fixed header + at least 1 item (6 bytes)
    if (sz < 34)
        return ErrorCode::CorruptVlr;

    uint16_t compressor, coder, revision, num_items;
    uint32_t options, chunk_size;
    int64_t  num_evlrs, offset_evlrs;

    std::memcpy(&compressor, d + 0,  2);
    std::memcpy(&coder,      d + 2,  2);
    out.version_major = d[4];
    out.version_minor = d[5];
    std::memcpy(&revision,   d + 6,  2);
    std::memcpy(&options,    d + 8,  4);
    std::memcpy(&chunk_size, d + 12, 4);
    std::memcpy(&num_evlrs,  d + 16, 8);
    std::memcpy(&offset_evlrs, d + 24, 8);
    std::memcpy(&num_items,  d + 32, 2);

    out.compressor = static_cast<LazCompressor>(compressor);
    out.coder = coder;
    out.version_revision = revision;
    out.options = options;
    out.chunk_size = chunk_size;
    out.num_special_evlrs = num_evlrs;
    out.offset_special_evlrs = offset_evlrs;

    if (coder != 0)
        return ErrorCode::LazNotSupported;  // only arithmetic coder

    // Validate item data fits
    if (sz < 34 + static_cast<uint64_t>(num_items) * 6)
        return ErrorCode::CorruptVlr;

    out.items.resize(num_items);
    for (uint16_t i = 0; i < num_items; ++i) {
        const uint8_t* item = d + 34 + i * 6;
        uint16_t type, item_size, item_version;
        std::memcpy(&type,         item + 0, 2);
        std::memcpy(&item_size,    item + 2, 2);
        std::memcpy(&item_version, item + 4, 2);
        out.items[i] = { static_cast<LazItemType>(type), item_size, item_version };
    }

    return ErrorCode::Ok;
}

} // namespace HLas::detail

#endif // HLAS_HAVE_LAZ
