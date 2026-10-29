#include "las_error.h"

namespace HLas::detail {

const char* error_message(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok:                     return "OK";
        case ErrorCode::FileNotFound:           return "File not found";
        case ErrorCode::FileOpenFailed:         return "Failed to open file";
        case ErrorCode::MmapFailed:             return "Memory mapping failed";
        case ErrorCode::InvalidSignature:       return "Invalid file signature (expected LASF)";
        case ErrorCode::UnsupportedVersion:     return "Unsupported LAS version";
        case ErrorCode::InvalidHeaderSize:      return "Invalid header size";
        case ErrorCode::UnsupportedPointFormat: return "Unsupported point data record format";
        case ErrorCode::InvalidPointRecordLength: return "Invalid point data record length";
        case ErrorCode::CorruptVlr:             return "Corrupt Variable Length Record";
        case ErrorCode::CorruptEvlr:            return "Corrupt Extended Variable Length Record";
        case ErrorCode::ReadPastEnd:            return "Read past end of file";
        case ErrorCode::LazNotSupported:        return "LAZ decompression not supported in this build";
        case ErrorCode::NotCopc:                return "Not a COPC file (no copc info VLR)";
        case ErrorCode::CorruptChunk:           return "Corrupt LAZ chunk";
    }
    return "Unknown error";
}

} // namespace HLas::detail
