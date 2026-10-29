#pragma once
#include "las_types.h"
#include "las_error.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace HLas::detail {

struct Vlr {
    char     user_id[16];
    uint16_t record_id;
    const uint8_t* data;      // Points into memory-mapped buffer (zero-copy)
    uint64_t data_size;        // uint16 for VLR, uint64 for EVLR
};

ErrorCode parse_vlrs(const uint8_t* buffer, size_t buffer_size,
                     size_t offset, uint32_t count,
                     std::vector<Vlr>& out);

ErrorCode parse_evlrs(const uint8_t* buffer, size_t buffer_size,
                      size_t offset, uint32_t count,
                      std::vector<Vlr>& out);

const Vlr* find_vlr(const std::vector<Vlr>& vlrs,
                     const char* user_id, uint16_t record_id);

} // namespace HLas::detail
