#pragma once
#ifdef HLAS_HAVE_LAZ

#include "arithmetic_encoder.h"
#include "integer_compressor.h"
#include <cstdint>

namespace HLas::detail::laz {

// GPSTime11-KODARE — spegel av GPSTime11Decoder, för LAS punktformat 1, 3, 4, 5.
//
// **Tillståndsmaskinen är densamma som i GPS-lagret i Point14Encoder, men
// SYMBOLNUMRERINGEN är det inte.** Point14 signalerar "tiden är oförändrad" med
// en BIT i changed_values och behöver ingen symbol för det; här finns ingen
// sådan bit. Nolldifferens-modellen har därför 6 symboler — 0 identisk, 1
// differens som ryms, 2 fullt värde, 3-5 sekvensbyte — mot Point14:s 5, där 0
// och 1 är de två senare och den första saknas. Multi-modellen har 516 symboler
// mot 515, med "oförändrad" på 511 och fullt värde på 512 mot 511.
//
// Multiplikatorsymbolerna 0-510 och det mesta däremellan är gemensamt, så en
// delad hjälpare med symbolnumreringen som parameter vore möjlig. Att den inte
// finns är ett underhållsval, inte ett korrekthetskrav — men det betyder att en
// rättelse i den ena grenen måste övervägas för den andra.
//
// Modellen: tiden hålls i FYRA referensfack. Ett steg kodas som en multipel av
// föregående steg i samma fack, vilket gör en jämnt pulsad skanner nästan
// gratis. Passar inget fack skrivs hela 64-bitarsvärdet och ett nytt fack tas i
// bruk.
//
// Kodningen kräver VAL som avkodningen bara läser — vilket fack, och vilken
// multiplikator — så det här är skrivet för att välja som referensens kodare
// gör, inte en spegling av vår egen avkodare.
class GPSTime11Encoder {
public:
    void init(ArithmeticEncoder& enc);

    // Chunkens första GPS-tid. De åtta byten skrivs RÅTT av anroparen.
    void start_chunk(const uint8_t* seed);

    // Kodar en GPS-tid. `item` är åtta byte, ett float64-bitmönster.
    void encode(const uint8_t* item);

    bool failed() const { return failed_ || (enc_ && enc_->failed()); }

private:
    ArithmeticEncoder* enc_ = nullptr;

    SymbolModel case_model_;    // 516 symboler
    SymbolModel case0_model_;   // 6 symboler
    IntegerCompressor ic_gps_;

    // Samma fyra fack som avkodaren håller.
    struct RefFrame {
        int64_t prev_gps_time = 0;
        int32_t delta = 0;       // int32: formatet trunkerar differensen till 32 bitar
        int32_t counter = 0;
    };
    RefFrame frames_[4];

    // TVÅ räknare, och det är inte en slarvighet. `last_` är facket vi räknar
    // ifrån och flyttas av ett sekvensbyte. `next_` är facket som tas i bruk
    // NÄSTA gång ett fullt värde skrivs, och räknas oberoende av `last_`.
    // Räknar man fram det nya facket ur `last_` i stället hamnar kodare och
    // avkodare i olika fack efter ett sekvensbyte följt av ett fullt värde.
    uint32_t last_ = 0;
    uint32_t next_ = 0;

    bool first_ = true;
    bool failed_ = false;
};

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
