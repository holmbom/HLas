#pragma once
#ifdef HLAS_HAVE_LAZ

#include "arithmetic.h"
#include <cassert>
#include <cstdint>
#include <vector>

namespace HLas::detail::laz {

// LAZ aritmetisk KODARE — spegel av ArithmeticDecoder i arithmetic.h.
//
// SymbolModel och BitModel återanvänds oförändrade. Det är inte bekvämlighet
// utan kravet: kodare och avkodare måste uppdatera modellerna bit-identiskt,
// annars glider strömmarna isär efter första divergensen. Delad kod kan inte
// glida — men den garanterar bara att uppdateringsREGELN är samma, inte att den
// sker vid samma tillfälle. Anropsordningen här speglar decodeSymbol/decodeBit
// rad för rad, och det är lika viktigt.
//
// Skillnaden mot avkodaren är carry-propagering. Avkodaren har ingen
// motsvarighet — den läser bara byte. Kodaren håller ett intervall [base,
// base+length) som kan spilla över 2^32, och då måste en etta läggas till
// bakåt genom redan utskrivna 0xFF-byte.
class ArithmeticEncoder {
public:
    void init();

    void encodeSymbol(SymbolModel& m, uint32_t sym);
    void encodeBit(BitModel& m, uint32_t bit);

    // bit_count måste vara 1..32.
    void writeBits(uint32_t bit_count, uint32_t sym);
    void writeShort(uint16_t v) { writeBits(16, v); }

    // Avkodarens readInt() gör två readBits(16), inte en readBits(32) — därför
    // två anrop här också.
    void writeInt(uint32_t v) {
        writeBits(16, v & 0xFFFFu);
        writeBits(16, v >> 16);
    }

    // Avslutar strömmen. Efter done() får inget mer kodas utan ny init().
    void done();

    const std::vector<uint8_t>& bytes() const { return out_; }
    size_t size() const { return out_.size(); }

    // Sätts om carry-propageringen inte hade någon byte att propagera till,
    // eller om ett kontraktsbrott upptäcktes i en release-build där assert är
    // bortkompilerad. Det ska inte kunna hända, men ett tyst fel här ger en fil
    // som avkodas till FEL PUNKTER i stället för att fela — därför måste varje
    // anropare kontrollera flaggan innan strömmen används.
    bool failed() const { return failed_; }

    // Låter en anropare markera strömmen som trasig utan att koda något. Finns
    // för att ett kontraktsbrott som upptäcks HOS ANROPAREN — där orsaken syns —
    // ska kunna stoppa strömmen där, i stället för att låta anroparen fortsätta
    // in i kod som inte längre har giltiga index.
    void fail() { failed_ = true; }

    // Antal carry-propageringar respektive antal 0xFF-byte de gått igenom.
    // Finns för att testsviten ska kunna KRÄVA att carry-vägen körs. Utan det
    // är "fuzzen är grön" förenligt med att den svåraste kodvägen aldrig nåtts,
    // och då bevisar den ingenting om just den. Kostnaden är en inkrementering
    // på en väg som redan är sällsynt.
    uint64_t carry_count() const { return carry_count_; }
    uint64_t carry_ff_count() const { return carry_ff_count_; }

private:
    // Efter done(), eller efter ett upptäckt kontraktsbrott, är strömmen inte
    // längre något man får skriva till. Kontrollen gäller ÄVEN i release: en
    // kodning efter done() skulle annars ändra base_, length_ och modellen utan
    // att någon märkte det, och resultatet blir en fil som avkodas till fel
    // punkter — precis det failed_ finns för att förhindra. Grenen är
    // välförutsägbar och försvinner i brus mot multiplikationerna nedan.
    bool usable() {
        if (done_ || failed_) { failed_ = true; return false; }
        return true;
    }

    void renorm();
    void propagate_carry();

    uint32_t base_   = 0;
    uint32_t length_ = 0;
    bool     failed_ = false;
    bool     done_   = false;
    uint64_t carry_count_    = 0;
    uint64_t carry_ff_count_ = 0;
    std::vector<uint8_t> out_;
};

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
