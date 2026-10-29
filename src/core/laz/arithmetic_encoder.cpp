#ifdef HLAS_HAVE_LAZ
#include "arithmetic_encoder.h"

namespace HLas::detail::laz {

void ArithmeticEncoder::init() {
    base_   = 0;
    length_ = AC_MAX_LENGTH;
    failed_ = false;
    done_   = false;
    carry_count_    = 0;
    carry_ff_count_ = 0;
    out_.clear();
}

// Intervallet [base, base+length) kan spilla över 2^32. base är uint32 och
// wrappar då, vilket upptäcks med `init_base > base`. Överspillet betyder att
// det tal som redan skrivits ut var ett för lågt, så en etta läggs till sist
// utskrivna byte — och 0xFF blir 0x00 med carry vidare bakåt.
//
// Att carryn aldrig kan gå ut ur HELA bufferten:
//
// Se strömmen som ett tal V i basen 256 med decimalkomma före första utskrivna
// byten — alltså de redan utskrivna byten PLUS base_ på rätt skala — och
// intervallbredden L på samma skala. Vid init() är V = 0 och L = 0xFFFFFFFF/2^32,
// alltså L <= 1, vilket är allt beviset behöver.
//
// Varje operation — encodeSymbol, encodeBit, writeBits — väljer ett DELintervall
// av det nuvarande, så V + L <= 1 bevaras. renorm() är värdebevarande: den
// flyttar översta byten ur base_ till prefixet och multiplicerar återstoden och
// L med 256, vilket är samma tal. Även done() håller sig innanför: den lägger
// till AC_MIN_LENGTH respektive AC_MIN_LENGTH/2, som båda ligger strikt under
// den övre gränsen i sin gren.
//
// En carry ut ur bufferten skulle betyda att V nådde 1, vilket motsäger
// V + L <= 1 med L > 0. Det täcker BÅDA fallen som ger i == 0: att ingen byte
// skrivits än, och att varje skriven byte var 0xFF.
//
// Kontrollen ligger kvar ändå: om resonemanget skulle vara fel någonstans ska
// det bli ett fel, inte en fil som tyst avkodas till andra punkter.
void ArithmeticEncoder::propagate_carry() {
    ++carry_count_;
    size_t i = out_.size();
    while (i > 0 && out_[i - 1] == 0xFFu) {
        out_[i - 1] = 0x00u;
        --i;
        ++carry_ff_count_;
    }
    if (i == 0) {
        failed_ = true;
        return;
    }
    ++out_[i - 1];
}

void ArithmeticEncoder::renorm() {
    do {
        out_.push_back(static_cast<uint8_t>(base_ >> 24));
        base_ <<= 8;
    } while ((length_ <<= 8) < AC_MIN_LENGTH);
}

// Spegel av decodeSymbol. Avkodaren väljer sym så att
// distribution[sym]*ltmp <= value och sätter length till intervallets bredd;
// här räknas samma två tal fram och läggs på base.
void ArithmeticEncoder::encodeSymbol(SymbolModel& m, uint32_t sym) {
    // Kontraktet kommer från våra egna kompressorer, inte från fil. En symbol
    // utanför modellen skulle läsa och skriva utanför distribution[] — assert
    // i debug, och failed_ i release så felet inte blir en tyst trasig ström.
    assert(m.num_symbols > 0 && sym < m.num_symbols);
    if (m.num_symbols == 0 || sym >= m.num_symbols) { failed_ = true; return; }
    if (!usable()) return;

    const uint32_t init_base = base_;
    uint32_t x;

    if (sym == m.num_symbols - 1) {
        // Sista symbolen har ingen övre gräns i tabellen — avkodaren gör
        // length = length - lower, alltså utan att skifta length.
        x = static_cast<uint32_t>(m.distribution[sym]) * (length_ >> DM_LENGTH_SHIFT);
        base_   += x;
        length_ -= x;
    } else {
        length_ >>= DM_LENGTH_SHIFT;
        x = static_cast<uint32_t>(m.distribution[sym]) * length_;
        base_   += x;
        length_  = static_cast<uint32_t>(m.distribution[sym + 1]) * length_ - x;
    }

    if (init_base > base_)
        propagate_carry();

    if (length_ < AC_MIN_LENGTH)
        renorm();

    m.symbol_count[sym]++;
    if (--m.symbols_until_update == 0)
        m.update_distribution();
}

// Spegel av decodeBit.
void ArithmeticEncoder::encodeBit(BitModel& m, uint32_t bit) {
    assert(bit <= 1);
    if (bit > 1) { failed_ = true; return; }
    if (!usable()) return;

    const uint32_t x = static_cast<uint32_t>(m.bit_0_prob) * (length_ >> BM_LENGTH_SHIFT);

    if (bit == 0) {
        length_ = x;
    } else {
        const uint32_t init_base = base_;
        base_   += x;
        length_ -= x;
        if (init_base > base_)
            propagate_carry();
    }

    if (length_ < AC_MIN_LENGTH)
        renorm();

    // Modelluppdateringen ligger EFTER renorm, precis som i decodeBit. Ordningen
    // spelar ingen roll för resultatet — renorm rör inte modellen — men en
    // spegel som avviker strukturellt tvingar nästa läsare att bevisa det på nytt.
    if (bit == 0)
        m.bit_0_count++;

    if (--m.bits_until_update == 0)
        m.update_bit_distribution();
}

// Spegel av readBits. Avkodaren läser låga 16 bitar först vid fler än 19, så
// de skrivs först här.
void ArithmeticEncoder::writeBits(uint32_t bit_count, uint32_t sym) {
    if (bit_count == 0 || bit_count > 32) {
        assert(false && "bit_count måste vara 1..32");
        failed_ = true;
        return;
    }
    // Ett värde som inte ryms i sin bredd kodas som de låga bitarna och avkodas
    // alltså till något annat. Det ska fångas här, inte upptäckas som avvikande
    // punkter långt senare.
    assert(bit_count == 32 || sym < (1u << bit_count));
    if (bit_count != 32 && sym >= (1u << bit_count)) { failed_ = true; return; }
    if (!usable()) return;

    if (bit_count > 19) {
        writeBits(16, sym & 0xFFFFu);
        sym       = sym >> 16;
        bit_count = bit_count - 16;
    }

    const uint32_t init_base = base_;
    length_ >>= bit_count;
    base_ += sym * length_;

    if (init_base > base_)
        propagate_carry();

    if (length_ < AC_MIN_LENGTH)
        renorm();
}

// Avslutningen väljer en punkt i det kvarvarande intervallet och spolar ut
// den. Exakt fyra byte hamnar i strömmen efter sista symbolen, oavsett gren:
// avkodaren läser fyra byte i init() och ligger alltså alltid steget före.
void ArithmeticEncoder::done() {
    assert(!done_);
    // Även done() måste vara inert efter ett fel. Annars kan ett avvisat
    // writeBits följas av en done() som ändå skriver en svans, och resultatet
    // blir en ström som ser färdig ut men saknar det som avvisades.
    if (done_ || failed_) { failed_ = true; return; }
    done_ = true;

    const uint32_t init_base = base_;
    bool another_byte = true;

    if (length_ > 2 * AC_MIN_LENGTH) {
        base_  += AC_MIN_LENGTH;        // en byte till räcker
        length_ = AC_MIN_LENGTH >> 1;
    } else {
        base_  += AC_MIN_LENGTH >> 1;   // två byte behövs
        length_ = AC_MIN_LENGTH >> 9;
        another_byte = false;
    }

    if (init_base > base_)
        propagate_carry();

    renorm();                           // skriver en eller två byte

    out_.push_back(0);
    out_.push_back(0);
    if (another_byte)
        out_.push_back(0);
}

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
