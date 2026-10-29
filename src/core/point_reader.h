#pragma once
#include "las_types.h"
#include "las_error.h"
#include "header_parser.h"
#include "point_source.h"
#include <cstddef>
#include <cstdint>

namespace HLas::detail {

class PointReader {
public:
    ErrorCode init(const ParsedHeader& header,
                   const uint8_t* file_buffer, size_t file_size);

    uint64_t count() const { return point_count_; }
    uint16_t record_length() const { return record_length_; }
    uint8_t format() const { return format_; }
    uint16_t extra_bytes_size() const { return extra_bytes_; }
    uint16_t classification_offset() const { return class_offset_; }

    // Vy över punktbufferten — den gemensamma nämnaren med LazReader och
    // SidecarReader. All läsning går genom den.
    PointSource point_source() const {
        return PointSource{ point_data_, point_count_, record_length_, class_offset_ };
    }

    // O(1) access to raw point record at index. Returns nullptr if out of range.
    const uint8_t* raw_point(uint64_t index) const { return point_source().raw_point(index); }

    // Batch read: memcpy count points starting at start_index into dest.
    // Returns actual number of points copied.
    uint64_t read_raw(uint64_t start_index, uint64_t count, uint8_t* dest) const {
        return point_source().read_raw(start_index, count, dest);
    }

private:
    const uint8_t* point_data_ = nullptr;
    uint64_t point_count_ = 0;
    uint16_t record_length_ = 0;
    uint8_t  format_ = 0;
    uint16_t extra_bytes_ = 0;
    uint16_t class_offset_ = 0;
};

} // namespace HLas::detail
