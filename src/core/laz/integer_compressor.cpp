#ifdef HLAS_HAVE_LAZ
#include "integer_compressor.h"
#include <cassert>

namespace HLas::detail::laz {

void IntegerCompressor::init_models(uint32_t bits, uint32_t num_instances) {
    bits_ = bits;
    bits_high_ = (bits < 8) ? bits : 8;
    num_instances_ = num_instances;
    last_k_ = 0;

    // corr_range and corr_min (spec 10.5)
    if (bits && bits < 32) {
        corr_range_ = static_cast<int32_t>(1u << bits);
        corr_min_ = -(corr_range_ >> 1);
        corr_max_ = corr_min_ + corr_range_ - 1;
    } else {
        corr_range_ = 0;       // overflows to 0 for 32-bit
        corr_min_ = INT32_MIN; // -2^31
        corr_max_ = INT32_MAX;
    }

    // One k-model per instance, each with (bits+1) symbols
    k_models_.assign(num_instances, SymbolModel{});
    for (auto& m : k_models_)
        m.init(bits + 1);

    // k=0 corrector: always a BitModel (shared across instances)
    bit_corrector_.init();

    // Corrector models for k=1..bits (shared across instances, spec 10.5.1)
    corrector_models_.assign(bits, SymbolModel{});
    for (uint32_t k = 1; k <= bits; ++k) {
        if (k <= bits_high_)
            corrector_models_[k - 1].init(1u << k);
        else
            corrector_models_[k - 1].init(1u << bits_high_); // 256 symbols
    }
}

void IntegerCompressor::init(ArithmeticDecoder& dec, uint32_t bits,
                             uint32_t num_instances) {
    init_models(bits, num_instances);
    dec_ = &dec;
    enc_ = nullptr;
}

void IntegerCompressor::init(ArithmeticEncoder& enc, uint32_t bits,
                             uint32_t num_instances) {
    init_models(bits, num_instances);
    enc_ = &enc;
    dec_ = nullptr;
}

int32_t IntegerCompressor::readCorrector(uint32_t context) {
    uint32_t k = dec_->decodeSymbol(k_models_[context]);
    last_k_ = k;

    int32_t c;
    if (k == 0) {
        c = static_cast<int32_t>(dec_->decodeBit(bit_corrector_));
    } else if (k >= 32) {
        // Kortslutningen gäller k == 32, INTE k == bits_.
        //
        // Den enda korrektor som ger k == bits_ är corr_min, och för bits_ == 32
        // är det INT32_MIN där hela informationen ligger i k självt — inget mer
        // skrivs och inget mer ska läsas. För bits_ < 32 skriver kodaren däremot
        // symbol och råbitar även för corr_min, och hoppar man över dem tappar
        // strömmen fasen: värdet blir rätt, men varje efterföljande punkt i
        // chunken blir fel.
        //
        // Verifierat mot referensen: en 16-bitars kompressor med korrektorn
        // corr_min gav 19 av 20 efterföljande värden fel före rättelsen, 0 efter.
        // Berör intensity, scan_angle och point_source, som alla är 16 bitar.
        c = corr_min_;
    } else {
        uint32_t corrector;
        if (k <= bits_high_) {
            corrector = dec_->decodeSymbol(corrector_models_[k - 1]);
        } else {
            uint32_t k1 = k - bits_high_;
            corrector = dec_->decodeSymbol(corrector_models_[k - 1]);
            corrector <<= k1;
            corrector |= dec_->readBits(static_cast<int>(k1));
        }

        // Sign conversion based on k (as the LAZ format defines it)
        if (corrector >= (1u << (k - 1))) {
            c = static_cast<int32_t>(corrector) + 1;
        } else {
            c = static_cast<int32_t>(corrector)
              - static_cast<int32_t>((1u << k) - 1);
        }
    }

    return c;
}

int32_t IntegerCompressor::decompress(int32_t pred, uint32_t context) {
    // pred + corrector kan spilla över int32 för en 32-bitars IC. Additionen
    // görs därför i uint32, av samma skäl som i ISum32.
    const int32_t corr = readCorrector(context);
    int32_t real = static_cast<int32_t>(static_cast<uint32_t>(pred) +
                                        static_cast<uint32_t>(corr));

    // Wrap for non-32-bit ICs (as the LAZ format requires)
    if (corr_range_) {
        if (real < 0)
            real += corr_range_;
        else if (static_cast<uint32_t>(real) >= static_cast<uint32_t>(corr_range_))
            real -= corr_range_;
    }

    return real;
}


// Spegel av readCorrector.
//
// k är antalet bitar som behövs för att beskriva korrektorn, och det räknas ut
// ur c på precis samma sätt som avkodaren sedan tolkar det: intervallet
// [-(2^k - 1), 2^k] är det tightaste som rymmer c.
void IntegerCompressor::writeCorrector(int32_t c, uint32_t context) {
    // Kontexten indexerar k_models_ direkt.
    assert(context < num_instances_);
    if (context >= num_instances_) { enc_->fail(); return; }

    // (c <= 0 ? -c : c-1). Negationen görs via int64 och inte på int32:
    // -INT32_MIN är signed overflow, alltså odefinierat beteende, och just
    // INT32_MIN är det värde som ska ge k = 32. Referensimplementationerna
    // förlitar sig på att det wrappar "i praktiken"; det gör vi inte.
    uint32_t c1 = (c <= 0) ? static_cast<uint32_t>(-static_cast<int64_t>(c))
                           : static_cast<uint32_t>(c - 1);
    uint32_t k = 0;
    while (c1) { c1 >>= 1; ++k; }
    last_k_ = k;

    // k > bits_ betyder att korrektorn inte rymdes i den här bredden. Det får
    // inte bara ge en trasig ström: k_models_ har bits_+1 symboler och
    // corrector_models_ bara bits_ poster, så en fortsättning skulle indexera
    // utanför båda. Kontrollen måste alltså gälla ÄVEN i release — att
    // encodeSymbol sätter failed_ hjälper inte, eftersom koden nedan ändå
    // fortsätter och indexerar.
    assert(k <= bits_);
    if (k > bits_) { enc_->fail(); return; }

    enc_->encodeSymbol(k_models_[context], k);

    if (k == 0) {
        // c är 0 eller 1.
        enc_->encodeBit(bit_corrector_, static_cast<uint32_t>(c));
        return;
    }

    // k == 32 betyder INT32_MIN, där hela informationen redan ligger i k.
    // Villkoret är 32 och inte bits_ — se readCorrector för vad skillnaden
    // kostade när den var fel.
    if (k == 32)
        return;

    // Flytta c till [0, 2^k - 1].
    uint32_t u;
    if (c < 0)
        u = static_cast<uint32_t>(c + static_cast<int32_t>((1u << k) - 1u));
    else
        u = static_cast<uint32_t>(c - 1);

    if (k <= bits_high_) {
        enc_->encodeSymbol(corrector_models_[k - 1], u);
    } else {
        const uint32_t k1 = k - bits_high_;
        enc_->encodeSymbol(corrector_models_[k - 1], u >> k1);
        enc_->writeBits(k1, u & ((1u << k1) - 1u));
    }
}

void IntegerCompressor::compress(int32_t pred, int32_t real, uint32_t context) {
    // Differensen räknas i uint32 och konverteras tillbaka. Subtraktionen ska
    // wrappa modulo 2^32 — det är algoritmen, inte en olycka — men att låta
    // signed int32 spilla över är odefinierat beteende. C++20 garanterar
    // tvåkomplement, så konverteringen tillbaka är väldefinierad.
    int32_t corr = static_cast<int32_t>(static_cast<uint32_t>(real) -
                                        static_cast<uint32_t>(pred));

    // Vik in i [corr_min, corr_max]. Ett steg räcker så länge pred och real
    // båda ligger i [0, corr_range), vilket är kontraktet för en IC med färre
    // än 32 bitar.
    if (corr_range_) {
        if (corr < corr_min_)      corr += corr_range_;
        else if (corr > corr_max_) corr -= corr_range_;
    }

    // Håller inte kontraktet — pred och real båda i [0, corr_range) — blir k
    // större än modellerna rymmer. Felet hör hemma här, där orsaken syns, och
    // det måste stoppa strömmen i release också.
    assert(corr >= corr_min_ && corr <= corr_max_);
    if (corr < corr_min_ || corr > corr_max_) { enc_->fail(); return; }

    writeCorrector(corr, context);
}

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
