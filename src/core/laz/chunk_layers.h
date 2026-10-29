#pragma once
#ifdef HLAS_HAVE_LAZ

#include "point14_encoder.h"
#include "rgb14_encoder.h"
#include <cstdint>
#include <vector>

namespace HLas::detail::laz {

// Sätter ihop en chunks lagerhuvud och lagerdata ur de items som ingår.
//
// Formatet kräver ALLA items lagerlängder först och sedan alla deras data, så
// ordningen är: Point14:s nio längder, RGB:s längd, Point14:s data, RGB:s data.
// Det går alltså inte att skriva ett item färdigt i taget, och det är därför
// den här funktionen finns i stället för två oberoende anrop.
//
// ALLT ELLER INGET. Felar något av itemen skrivs ingenting till `out` och
// funktionen returnerar false. Utan den garantin kan halva chunken hamna i
// bufferten: felar RGB men inte Point14 blir längden noll medan Point14
// skriver som vanligt, och resultatet är en chunk som AVKODAS UTAN FEL men ger
// fel färger — tyst, och först synligt för den som tittar på bilden. Felar
// Point14 men inte RGB blir chunken i stället strukturellt trasig.
//
// `rgb` är nullptr för punktformat 6, som inte har något färgitem.
inline bool append_chunk_layers(std::vector<uint8_t>& out,
                                Point14Encoder& p14,
                                RGB14Encoder* rgb) {
    p14.finish_chunk();
    if (rgb) rgb->finish_chunk();

    // Kontrollen måste ske FÖRE första append, inte efter — det är det som gör
    // operationen atomisk utan att behöva en mellanbuffert.
    if (p14.failed() || (rgb && rgb->failed()))
        return false;

    p14.append_sizes(out);
    if (rgb) rgb->append_size(out);
    p14.append_data(out);
    if (rgb) rgb->append_data(out);
    return true;
}

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
