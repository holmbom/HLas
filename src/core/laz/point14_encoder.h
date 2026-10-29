#pragma once
#ifdef HLAS_HAVE_LAZ

#include "arithmetic_encoder.h"
#include "integer_compressor.h"
#include "point14.h"
#include "point14_item.h"
#include <cstdint>
#include <vector>

namespace HLas::detail::laz {

// Punktformat 6 så som HLas skriver det i LITE-läge: X, Y, Z och klassificering.
//
// Att kodaren TAR EMOT den här strukturen och inte bara en rå 30-bytespost är
// grunden för att sex av nio lager kan lämnas tomma i LITE. Fälten som inte
// finns här — intensitet, scanvinkel, user data, point source, GPS-tid och
// flaggorna — kan inte anta något annat värde än noll, eftersom det inte går
// att uttrycka. En assertion hade kompilerats bort i release; det här kan inte.
struct Point14Fields {
    int32_t x = 0, y = 0, z = 0;
    uint8_t classification = 0;
};

// Snubbeltråd. Typen ensam RÄCKER INTE som framtidsskydd: lägger någon till ett
// fält fortsätter to_item() och encode() att kompilera och ignorerar det tyst,
// och ett nytt uint8_t ryms dessutom i den befintliga utfyllnaden så inte ens
// sizeof ändras.
//
// En structured binding kräver däremot exakt rätt antal medlemmar. Ett femte
// fält gör att den här funktionen inte kompilerar, och den som lägger till det
// tvingas hit — där svaret är att fältet inte hör hemma i LITE alls. Vill man
// bära det, använd FULL-vägen: encode_item() tar hela posten.
inline void point14_fields_tripwire() {
    Point14Fields f{};
    auto& [x, y, z, classification] = f;
    (void)x; (void)y; (void)z; (void)classification;
}

// Point14 v3-KODARE, spegel av Point14Decoder.
//
// EN kodare med två ingångar, inte två kodare:
//
//   LITE   start_chunk(Point14Fields) + encode(Point14Fields)
//   FULL   start_chunk_item(const uint8_t*) + encode_item(const uint8_t*)
//
// LITE bygger en 30-bytespost av sina fyra fält och går sedan exakt samma väg.
// Två separata kodare hade varit samma fel som IntegerCompressor uttryckligen
// skyddar mot genom att dela klass mellan läsning och skrivning: två kopior kan
// glida isär, en kan inte.
//
// **Tillståndet är Point14Context — samma struct som avkodaren använder.** Alla
// modeller, medianfönster och senast-värden ligger där, och att de delas är av
// samma skäl. De fyra kontexterna är scannerkanalerna.
//
// LAGERPOLICYN, avläst ur referensens field_point14.cpp och inte antagen:
//
//   channel_returns_XY  alltid            (xy_enc_ = true)
//   Z                   alltid            (z_enc_ = true)
//   classification      kodas för VARJE punkt, lagret skrivs om något ändrats
//   flags               likadant
//   intensity           likadant
//   user_data           likadant
//   scan_angle          kodas BARA när värdet ändrats
//   point_source        kodas BARA när värdet ändrats
//   gps_time            kodas BARA när värdet ändrats
//
// Skillnaden mellan de två grupperna är inte kosmetisk. De fyra första måste
// kodas varje punkt för att modellerna ska följa avkodarens, som läser varje
// punkt så snart lagret finns. De tre sista har en egen ändringsbit i
// changed_values, så avkodaren vet när den ska läsa.
//
// GPS-TIDEN väljer som referensens kodare gör och är inte en spegling av
// avkodaren. Skillnaden är verklig: avkodningen LÄSER ett multi-värde, medan
// kodningen måste VÄLJA det — vilken av fyra sekvenser som ska användas, och
// vilken multiplikator. Det valet går inte att härleda ur avkodaren.
class Point14Encoder {
public:
    // Börjar en ny chunk. Seed-posten skrivs RÅ av anroparen och kodas inte.
    //
    // Nollställer allt, inklusive failed_ — en ny chunk är en ny start. Men den
    // vägrar starta över en chunk som redan är igång: det hade tyst kastat bort
    // det som kodats så långt.
    void start_chunk(const Point14Fields& seed);
    void start_chunk_item(const uint8_t* seed_item);

    // Kodar en punkt. Får anropas fritt många gånger efter start_chunk.
    void encode(const Point14Fields& p);
    void encode_item(const uint8_t* item);

    // APPENDERAR de nio lagerlängderna följt av lagerdata till `out` — den
    // rensas inte. Efter anropet måste start_chunk anropas igen innan nästa
    // punkt. Har något gått fel skrivs ingenting, så en trasig chunk aldrig kan
    // registreras som giltig av anroparen.
    void finish_chunk(std::vector<uint8_t>& out);

    // Samma sak i tre steg, för punktformat 7 och uppåt. Formatet kräver ALLA
    // items lagerlängder först och sedan alla deras data, så RGB:s längd ligger
    // mellan Point14:s längder och Point14:s data — det går inte att skriva
    // Point14 färdigt i ett svep.
    void finish_chunk();
    void append_sizes(std::vector<uint8_t>& out) const;
    void append_data(std::vector<uint8_t>& out) const;

    bool failed() const { return failed_; }

    // Lagerhuvudet, samma konstant som avkodaren använder.
    static constexpr uint32_t kChunkSizeHeaderBytes = 9 * 4;

private:
    void init_context(uint32_t ctx, const uint8_t* seed_item);
    void encode_gps_time(Point14Context& c, double gps);

    // Samma tillstånd som avkodaren, en uppsättning per scannerkanal.
    Point14Context contexts_[4];
    uint32_t       current_context_ = 0;

    ArithmeticEncoder enc_xy_, enc_z_, enc_class_, enc_flags_, enc_intensity_,
                      enc_scan_angle_, enc_user_data_, enc_point_source_, enc_gps_;

    // Referensens makeValid(): lagret finns i strömmen, men skrivs bara ut om
    // något faktiskt ändrats. Nollängd betyder "oförändrat sedan seed" för
    // avkodaren, och det är den mekanismen som gör att en fil med konstant
    // user_data inte betalar för lagret.
    bool valid_class_ = false, valid_flags_ = false, valid_intensity_ = false,
         valid_scan_angle_ = false, valid_user_data_ = false,
         valid_point_source_ = false, valid_gps_ = false;

    bool in_chunk_ = false;
    bool failed_ = false;

    // Sätts av finish_chunk() så append_sizes/append_data kan vara const.
    uint32_t sizes_[9] = {};
};

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
