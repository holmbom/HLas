#pragma once
#ifdef HLAS_HAVE_LAZ

#include "arithmetic.h"
#include "arithmetic_encoder.h"
#include <cstdint>
#include <vector>

namespace HLas::detail::laz {

// LAZ Integer Compressor / Decompressor (spec 10.5)
//
// As the LAZ format defines it: decompress(pred, context) returns
// pred + readCorrector(context), with wrapping for non-32-bit ICs.
//
// Corrector models are created for ALL bit sizes:
//   k=0: BitModel (single bit)
//   k=1..8: SymbolModel with 2^k symbols
//   k=9..bits: SymbolModel with 256 symbols + (k-8) raw lower bits
// These corrector models are shared across all instances (spec 10.5.1).

class IntegerCompressor {
public:
    // En instans är antingen läsande eller skrivande, aldrig båda. Modellerna
    // sätts upp identiskt i båda fallen — det är hela poängen med att de delar
    // klass i stället för att finnas i två kopior som kan glida isär.
    void init(ArithmeticDecoder& dec, uint32_t bits, uint32_t num_instances);
    void init(ArithmeticEncoder& enc, uint32_t bits, uint32_t num_instances);

    // Decompress one value: returns pred + corrector (with wrapping for <32-bit)
    int32_t decompress(int32_t pred, uint32_t context = 0);

    // Spegel av decompress. `real` är värdet före invikningen som decompress
    // gör, alltså precis det som decompress kommer att returnera.
    void compress(int32_t pred, int32_t real, uint32_t context = 0);

    uint32_t last_k() const { return last_k_; }

private:
    void init_models(uint32_t bits, uint32_t num_instances);
    int32_t readCorrector(uint32_t context);
    void    writeCorrector(int32_t c, uint32_t context);

    ArithmeticDecoder* dec_ = nullptr;
    ArithmeticEncoder* enc_ = nullptr;
    uint32_t bits_ = 0;
    uint32_t bits_high_ = 0;            // min(bits, 8)
    uint32_t num_instances_ = 0;
    uint32_t last_k_ = 0;
    int32_t  corr_range_ = 0;           // 1 << bits (0 for bits=32)
    int32_t  corr_min_ = 0;             // -(corr_range/2) or INT32_MIN
    int32_t  corr_max_ = 0;             // corr_min + corr_range - 1, or INT32_MAX

    std::vector<SymbolModel> k_models_;  // one per instance
    BitModel bit_corrector_;             // k=0 corrector (shared)
    std::vector<SymbolModel> corrector_models_;  // k=1..bits (shared)
};

// ---- Definierad moduloaritmetik (spec 10.5.4) ----
//
// Alla summor och produkter här SKA wrappa — det är algoritmen. Signed overflow
// är däremot odefinierat beteende, inte "wrappar i praktiken", så räkningen görs
// i unsigned och konverteras tillbaka. C++20 garanterar tvåkomplement, så
// bitmönstret blir exakt det avsedda.

inline int32_t ISum32(int32_t pred, int32_t c) {
    // Summan SKA wrappa modulo 2^32 — det är algoritmen — men signed overflow
    // är odefinierat beteende, inte "wrappar i praktiken". Räkna i uint32 och
    // konvertera tillbaka; C++20 garanterar tvåkomplement, så konverteringen
    // är väldefinierad och ger exakt samma bitmönster som avsikten.
    return static_cast<int32_t>(static_cast<uint32_t>(pred) +
                                static_cast<uint32_t>(c));
}

// Multiplikation som wrappar modulo 2^32, av samma skäl som ISum32.
//
// Semantiken är int32 med wrappning, INTE int64: referensimplementationerna
// multiplicerar två int32 och låter resultatet spilla över. Räknar man i int64
// i stället får man ett annat prediktionsvärde, och då avkodas filen fel — det
// är alltså inte "säkrare", det är fel.
inline int32_t IMul32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) *
                                static_cast<uint32_t>(b));
}

// GPS-tiden är ett int64-bitmönster som kommer RÅTT UR FILEN via seed-posten.
// En fil kan alltså sätta den nära INT64_MAX och få nästa addition att spilla
// över. Det är ingen realistisk GPS-tid, men avkodaren läser filer vi inte
// kontrollerar och får inte ha odefinierat beteende för någon indata.
// Differens som wrappar modulo 2^32. Motstycket till ISum32: avkodaren gör
// x = ISum32(last_x, diff), så kodaren måste bilda diff = x - last_x med samma
// wrappning, annars stämmer inte det ena mot det andra vid överspill.
inline int32_t IDiff32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) -
                                static_cast<uint32_t>(b));
}

// Differens mellan två int64 som wrappar modulo 2^64. Motstycket till ISum64.
//
// Behövs för GPS-tiden, som räknas på BITMÖNSTER och inte som tal. Två giltiga
// doubles kan ha bitmönster i var sin ände av int64-området, och skillnaden ryms
// då inte — referensimplementationerna subtraherar rakt av och har odefinierat
// beteende där.
//
// Wrappningen är INTE oskyldig: INT64_MIN - INT64_MAX ger 1 modulo 2^64, alltså
// ett helt annat svar än den matematiska differensen. Men det är samma svar som
// tvåkomplement ger i praktiken, och det är den aritmetiken formatet bygger på —
// differensen används både för att avgöra om den ryms i en int32 och, när den
// gör det, som det värde som kodas.
inline int64_t IDiff64(int64_t a, int64_t b) {
    return static_cast<int64_t>(static_cast<uint64_t>(a) -
                                static_cast<uint64_t>(b));
}

inline int64_t ISum64(int64_t pred, int64_t c) {
    return static_cast<int64_t>(static_cast<uint64_t>(pred) +
                                static_cast<uint64_t>(c));
}

inline uint16_t ISum16(uint16_t pred, int32_t c) {
    int32_t v = static_cast<int32_t>(pred) + c;
    if (v < 0) v += 65536;
    else if (v >= 65536) v -= 65536;
    return static_cast<uint16_t>(v);
}

inline uint8_t ISum8(uint8_t pred, int32_t c) {
    int32_t v = static_cast<int32_t>(pred) + c;
    if (v < 0) v += 256;
    else if (v >= 256) v -= 256;
    return static_cast<uint8_t>(v);
}

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
