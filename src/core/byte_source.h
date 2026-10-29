#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

namespace HLas::detail {

// Källa till råa byte ur en LAS/LAZ-fil, oavsett var den ligger.
//
// Två implementationer är tänkta: FileByteSource över en mmap:ad fil (core) och
// en HTTP-variant över range requests (net/, Fas 15). Kod som läser COPC-index
// och chunkar ska gå genom den här och inte veta vilken det är.
//
// Till skillnad från PointSource är den *virtuell*, och det är avsiktligt: här
// finns verklig beteendeskillnad att specialisera — en memcpy ur mmap mot en
// nätverksförfrågan — och anropen sker per chunk, alltså hundratals gånger, inte
// per punkt miljontals gånger. Kostnaden för ett indirekt anrop är försumbar
// bredvid det arbete varje anrop utför.
class ByteSource {
public:
    virtual ~ByteSource() = default;

    // Läser exakt `len` byte från `offset` till `dest`.
    // Returnerar false om intervallet ligger utanför källan eller om läsningen
    // misslyckades. Vid false är innehållet i `dest` odefinierat.
    virtual bool read(uint64_t offset, size_t len, uint8_t* dest) = 0;

    // Källans totala storlek i byte.
    virtual uint64_t size() const = 0;

    // En oberoende källa mot samma data, för användning i en annan tråd.
    //
    // Behövs för att hämta flera noder parallellt: en HTTP-källa äger ett
    // uppkopplingshandtag och kan inte delas mellan trådar, och samtidiga
    // förfrågningar över en uppkoppling ger ändå ingen genomströmning — mätt
    // mot S3 gav fyra uppkopplingar 6x, en enda 1x.
    //
    // Returnerar nullptr när källan inte kan klonas. Anroparen faller då
    // tillbaka på seriell körning i stället för att misslyckas.
    virtual std::unique_ptr<ByteSource> clone() const { return nullptr; }

    // Intervallkontroll i subtraktionsform. `offset + len` kan wrappa i uint64_t
    // och då släppa igenom ett intervall som ligger långt utanför källan — det
    // här uttrycket kan inte det.
    //
    // Varje implementation MÅSTE gå igenom den här innan den läser. Anropare
    // som parse_copc_hierarchy vet inte hur stor källan är, och ska inte behöva
    // veta det — över HTTP är storleken inte ens känd lokalt.
    static bool range_ok(uint64_t offset, uint64_t len, uint64_t total) {
        return offset <= total && len <= total - offset;
    }

protected:
    ByteSource() = default;
    ByteSource(const ByteSource&) = default;
    ByteSource& operator=(const ByteSource&) = default;
};

} // namespace HLas::detail
