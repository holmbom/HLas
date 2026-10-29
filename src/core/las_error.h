#pragma once
#include <cstdint>

namespace HLas::detail {

enum class ErrorCode : uint8_t {
    Ok = 0,
    FileNotFound,
    FileOpenFailed,
    MmapFailed,
    InvalidSignature,
    UnsupportedVersion,
    InvalidHeaderSize,
    UnsupportedPointFormat,
    InvalidPointRecordLength,
    CorruptVlr,
    CorruptEvlr,
    ReadPastEnd,
    LazNotSupported,
    NotCopc,
    CorruptChunk,
};

const char* error_message(ErrorCode code);

} // namespace HLas::detail
