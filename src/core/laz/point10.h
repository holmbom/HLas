#pragma once
#ifdef HLAS_HAVE_LAZ

#include "arithmetic.h"
#include "integer_compressor.h"
#include "streaming_median.h"
#include "point10_item.h"
#include <cstdint>
#include <cstring>

namespace HLas::detail::laz {

// Point10 decoder — LAS formats 0-5, common 20-byte core (spec 13.1)
class Point10Decoder {
public:
    void init(ArithmeticDecoder& dec);

    // Decompress one point into dest (20 bytes: X,Y,Z,intensity,flags,class,scan_angle,user_data,point_source_id)
    void decompress(const uint8_t* seed, uint8_t* dest);

private:
    ArithmeticDecoder* dec_ = nullptr;

    // Changed values: 64 symbols (6 bits)
    SymbolModel changed_values_model_;

    // Bit-Byte: 256 instances × 256 symbols
    SymbolModel bit_byte_models_[256];

    // dIntensity: IC 16-bit, 4 instances
    IntegerCompressor ic_intensity_;

    // Classification: 256 instances × 256 symbols
    SymbolModel classification_models_[256];

    // dScan Angle Rank: 2 instances × 256 symbols
    SymbolModel scan_angle_models_[2];

    // User Data: 256 instances × 256 symbols
    SymbolModel user_data_models_[256];

    // dPoint Source ID: IC 16-bit, 1 instance
    IntegerCompressor ic_point_source_;

    // dX: IC 32-bit, 2 instances
    IntegerCompressor ic_dx_;

    // dY: IC 32-bit, 22 instances
    IntegerCompressor ic_dy_;

    // dZ: IC 32-bit, 20 instances
    IntegerCompressor ic_dz_;

    // Streaming medians for dX and dY (16 each, indexed by m)
    StreamingMedian median_x_[16];
    StreamingMedian median_y_[16];

    // Previous Z per return level (8 values)
    int32_t prev_z_[8] = {};

    // Previous intensity per m-class (16, but only 4 used)
    uint16_t prev_intensity_[16] = {};

    // Previous point record (20 bytes)
    uint8_t prev_[20] = {};
    bool    first_ = true;
};

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
