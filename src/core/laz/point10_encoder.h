#pragma once
#ifdef HLAS_HAVE_LAZ

#include "arithmetic_encoder.h"
#include "integer_compressor.h"
#include "streaming_median.h"
#include <cstdint>
#include <cstring>

namespace HLas::detail::laz {

// Point10-KODARE — spegel av Point10Decoder, för LAS punktformat 0-5.
//
// **Den här kodaren delar INTE ström med sig själv per fält.** Point14 delar
// upp chunken i nio lager med var sin aritmetiska ström; Point10 gör inget av
// det. Alla items i en format 0-5-chunk skriver i EN gemensam ström, punkt för
// punkt, i den ordning items står i LAZ-VLR:n. Därför tar init() en referens
// till en kodare som ägs av någon annan, i stället för att äga sin egen.
//
// Modellen är densamma som avkodarens: en 6-bitars mask säger vilka fält som
// ändrats sedan föregående punkt, och bara de kodas. Koordinaterna kodas alltid,
// som skillnad mot föregående punkt minus den löpande medianen av de fem senaste
// skillnaderna för samma retur-klass.
class Point10Encoder {
public:
    void init(ArithmeticEncoder& enc);

    // Chunkens första punkt. Dess tjugo byte skrivs RÅTT av anroparen, före
    // strömmen — kodaren tar bara emot dem som utgångsläge.
    //
    // Att seed-punktens intensitet och höjd INTE blir utgångsläge är avsiktligt
    // och delas med avkodaren: last_intensity och last_height står kvar på noll.
    // Referensen gör likadant, och en kodare som "rättar" det ger en fil vår
    // egen avkodare läser fel.
    void start_chunk(const uint8_t* seed);

    // Kodar en punkt. `item` är tjugo byte i LAS-postens layout.
    void encode(const uint8_t* item);

    bool failed() const { return failed_ || (enc_ && enc_->failed()); }

private:
    ArithmeticEncoder* enc_ = nullptr;

    SymbolModel changed_values_model_;
    SymbolModel bit_byte_models_[256];
    SymbolModel classification_models_[256];
    SymbolModel scan_angle_models_[2];
    SymbolModel user_data_models_[256];

    IntegerCompressor ic_intensity_;
    IntegerCompressor ic_point_source_;
    IntegerCompressor ic_dx_;
    IntegerCompressor ic_dy_;
    IntegerCompressor ic_dz_;

    StreamingMedian median_x_[16];
    StreamingMedian median_y_[16];

    int32_t  prev_z_[8] = {};
    uint16_t prev_intensity_[16] = {};
    uint8_t  prev_[20] = {};
    bool     first_ = true;
    bool     failed_ = false;
};

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
