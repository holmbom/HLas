#include "point_reader.h"
#include <cstring>

namespace HLas::detail {

ErrorCode PointReader::init(const ParsedHeader& header,
                            const uint8_t* file_buffer, size_t file_size) {
    format_ = header.point_format_id;
    record_length_ = header.point_record_length;
    point_count_ = header.point_count;

    if (format_ > 10)
        return ErrorCode::UnsupportedPointFormat;

    extra_bytes_ = record_length_ - kMinPointSize[format_];
    class_offset_ = (format_ <= 5) ? 15 : 16;

    // Validate that all point data fits in the file
    uint64_t data_end = static_cast<uint64_t>(header.offset_to_point_data)
                      + point_count_ * record_length_;
    if (data_end > file_size)
        return ErrorCode::ReadPastEnd;

    point_data_ = file_buffer + header.offset_to_point_data;

    return ErrorCode::Ok;
}

} // namespace HLas::detail
