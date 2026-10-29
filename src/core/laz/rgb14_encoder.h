#pragma once
#ifdef HLAS_HAVE_LAZ

#include "arithmetic_encoder.h"
#include "rgb14.h"
#include <cstdint>
#include <vector>

namespace HLas::detail::laz {

// RGB14 v3-KODARE, spegel av RGB14Decoder.
//
// Färgen kodas som skillnader mot föregående punkt, byte för byte: en
// symbolmask säger vilka av de sex halvbyten som ändrats, och bara de kodas.
// Är R, G och B lika kodas bara R — det är bit 6 i masken, och därför är en grå
// bild ungefär hälften så stor som en färgad.
//
// Grönt och blått predikteras dessutom ur RÖDS ändring: har rött blivit
// ljusare har troligen grönt också. Blått predikteras ur medelvärdet av rödens
// och grönens ändring. Det är hela vinsten i lagret.
//
// Tillståndet är RGB14Context — samma struct som avkodaren använder.
class RGB14Encoder {
public:
    // Seed-posten är de sex RGB-byten ur chunkens första punkt, som skrivs rått
    // av anroparen. `context` är seed-punktens scannerkanal.
    void start_chunk(const uint8_t* seed_rgb, uint32_t context);

    // `item` är sex byte: R, G, B som uint16 var. `context` är punktens
    // scannerkanal, som Point14-kodaren redan räknat fram.
    void encode(uint32_t context, const uint8_t* item);

    // Avslutar strömmen. Måste anropas före append_size/append_data.
    void finish_chunk();

    // Lagrets längd (en uint32) respektive dess byte. Skilda för att formatet
    // kräver ALLA items lagerlängder först och sedan alla deras data — RGB:s
    // längd ligger efter Point14:s nio, men RGB:s data efter Point14:s data.
    void append_size(std::vector<uint8_t>& out) const;
    void append_data(std::vector<uint8_t>& out) const;

    bool failed() const { return failed_; }

    static constexpr uint32_t kChunkSizeHeaderBytes = 4;

private:
    void init_context(uint32_t ctx, const uint16_t* seed);

    RGB14Context      contexts_[4];
    uint32_t          current_context_ = 0;
    ArithmeticEncoder enc_;

    // Referensens makeValid(): lagret skrivs bara om någon punkt faktiskt har en
    // annan färg än sin föregångare. En chunk med enfärgade punkter kostar noll.
    bool valid_ = false;
    bool in_chunk_ = false;
    bool failed_ = false;
};

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
