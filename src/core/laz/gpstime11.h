#pragma once
#ifdef HLAS_HAVE_LAZ

#include "arithmetic.h"
#include "integer_compressor.h"
#include <cstdint>

namespace HLas::detail::laz {

// GPSTime11 decoder — 8-byte GPS time (spec 13.2)
class GPSTime11Decoder {
public:
    void init(ArithmeticDecoder& dec);
    void decompress(const uint8_t* seed, uint8_t* dest);

private:
    ArithmeticDecoder* dec_ = nullptr;

    // case symbol: 516 symbols, 1 instance
    SymbolModel case_model_;

    // case_0delta: 6 symbols, 1 instance
    SymbolModel case0_model_;

    // dGPS Time (low): IC 32-bit, 9 instances
    IntegerCompressor ic_gps_;

    // 4 reference frames
    struct RefFrame {
        int64_t  prev_gps_time = 0;
        int32_t  delta = 0;       // int32_t: the format truncates the delta to 32 bits
        int32_t  counter = 0;
    };
    RefFrame frames_[4];

    // TVÅ räknare, och det är inte överflödigt. `current_frame_` är facket vi
    // räknar ifrån och flyttas av ett sekvensbyte. `next_frame_` pekar ut det
    // fack som tas i bruk NÄSTA gång ett fullt värde läses, och räknas oberoende
    // av det första. De är lika ända tills ett sekvensbyte skilt dem åt — och
    // FÖRST då, vid nästa fulla värde, ger den ena räknaren fel fack.
    //
    // Avkodaren räknade tidigare fram det nya facket ur current_frame_ och gav
    // därför fel GPS-tider i filer som innehåller ett sekvensbyte följt av ett
    // fullt värde. Hittad av gyllene vektorer ur referensen, som faller
    // på tre av tre GPS-fall utan det här.
    uint32_t current_frame_ = 0;
    uint32_t next_frame_ = 0;

    bool first_ = true;
};

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
