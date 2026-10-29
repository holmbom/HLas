#pragma once
#ifdef HLAS_HAVE_LAZ

#include "arithmetic.h"
#include "integer_compressor.h"
#include "streaming_median.h"
#include <cstdint>
#include <cstring>

namespace HLas::detail::laz {

// GPS time constants (as the LAZ format requires)
constexpr int32_t GPSTIME_MULTI       = 500;
constexpr int32_t GPSTIME_MULTI_MINUS = -10;
constexpr int32_t GPSTIME_MULTI_CODE_FULL = GPSTIME_MULTI - GPSTIME_MULTI_MINUS + 1; // 511
constexpr int32_t GPSTIME_MULTI_TOTAL     = GPSTIME_MULTI - GPSTIME_MULTI_MINUS + 5; // 515

// Return map/level tables for 16 returns (format 6-10)
extern const uint8_t number_return_map_6ctx[16][16];
extern const uint8_t number_return_level_8ctx[16][16];

// Per-scanner-channel context for Point14
struct Point14Context {
    bool unused = true;

    uint8_t last_item[30] = {};       // last decompressed point
    uint16_t last_intensity[8] = {};  // indexed by (cpr<<1)|gps_time_change
    StreamingMedian last_X_diff_median5[12]; // indexed by (m<<1)|gps_time_change
    StreamingMedian last_Y_diff_median5[12];
    int32_t last_Z[8] = {};           // indexed by return level l

    // Models for channel_returns_XY layer
    SymbolModel m_changed_values[8];  // indexed by lpr (3-bit)
    SymbolModel m_scanner_channel;    // 3 symbols
    SymbolModel m_number_of_returns[16]; // lazy, indexed by last_n
    SymbolModel m_return_number_gps_same; // 13 symbols
    SymbolModel m_return_number[16];  // lazy, indexed by last_r
    IntegerCompressor ic_dX;
    IntegerCompressor ic_dY;

    // Z layer
    IntegerCompressor ic_Z;

    // Classification layer
    SymbolModel m_classification[64]; // indexed by (cls&0x1F)<<1 | (cpr==3)

    // Flags layer
    SymbolModel m_flags[64];          // indexed by last_flags

    // Intensity layer
    IntegerCompressor ic_intensity;

    // Scan angle layer
    IntegerCompressor ic_scan_angle;

    // User data layer
    SymbolModel m_user_data[64];      // indexed by last_user_data/4

    // Point source layer
    IntegerCompressor ic_point_source;

    // GPS time layer
    uint32_t last = 0, next = 0;
    union U64I64F64 { uint64_t u64; int64_t i64; double f64; };
    U64I64F64 last_gpstime[4] = {};
    int32_t last_gpstime_diff[4] = {};
    int32_t multi_extreme_counter[4] = {};
    SymbolModel m_gpstime_multi;
    SymbolModel m_gpstime_0diff;
    IntegerCompressor ic_gpstime;

    bool gps_time_change = false;
};

// Point14 v3/v4 decoder (LAS 1.4, format 6-10, LayerChunked)
class Point14Decoder {
public:
    // Read 9 layer byte sizes from stream, advance ptr
    void read_chunk_sizes(const uint8_t*& ptr);

    // Lagerhuvudets storlek (9 x uint32) och den nyttolast som read_chunk_sizes
    // deklarerade. Summan görs i uint64 — de nio uint32-värdena kommer ur filen
    // och kan tillsammans spilla över 32 bitar.
    static constexpr uint64_t kChunkSizeHeaderBytes = 9 * 4;
    uint64_t declared_layer_bytes() const {
        return uint64_t(num_bytes_channel_returns_XY_) + num_bytes_Z_ +
               num_bytes_classification_ + num_bytes_flags_ +
               num_bytes_intensity_ + num_bytes_scan_angle_ +
               num_bytes_user_data_ + num_bytes_point_source_ +
               num_bytes_gps_time_;
    }

    // Read layer bytes, init per-layer decoders, set up context from seed
    void init_chunk(const uint8_t*& ptr, const uint8_t* seed);

    // Decompress one point. context is in/out (scanner channel).
    void decompress(uint32_t& context, uint8_t* dest);

private:
    void init_context(uint32_t ctx, const uint8_t* seed_item);
    void read_gps_time();

    // Layer byte sizes (from chunk_sizes)
    uint32_t num_bytes_channel_returns_XY_ = 0;
    uint32_t num_bytes_Z_ = 0;
    uint32_t num_bytes_classification_ = 0;
    uint32_t num_bytes_flags_ = 0;
    uint32_t num_bytes_intensity_ = 0;
    uint32_t num_bytes_scan_angle_ = 0;
    uint32_t num_bytes_user_data_ = 0;
    uint32_t num_bytes_point_source_ = 0;
    uint32_t num_bytes_gps_time_ = 0;

    // Layer byte buffer
    std::vector<uint8_t> layer_bytes_;

    // Per-layer arithmetic decoders
    ArithmeticDecoder dec_channel_returns_XY_;
    ArithmeticDecoder dec_Z_;
    ArithmeticDecoder dec_classification_;
    ArithmeticDecoder dec_flags_;
    ArithmeticDecoder dec_intensity_;
    ArithmeticDecoder dec_scan_angle_;
    ArithmeticDecoder dec_user_data_;
    ArithmeticDecoder dec_point_source_;
    ArithmeticDecoder dec_gps_time_;

    // Layer changed flags
    bool changed_Z_ = false;
    bool changed_classification_ = false;
    bool changed_flags_ = false;
    bool changed_intensity_ = false;
    bool changed_scan_angle_ = false;
    bool changed_user_data_ = false;
    bool changed_point_source_ = false;
    bool changed_gps_time_ = false;

    // Scanner channel contexts (4)
    Point14Context contexts_[4];
    uint32_t current_context_ = 0;
};

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
