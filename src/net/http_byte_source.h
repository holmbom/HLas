#pragma once
#ifdef HLAS_HAVE_NET

#include "byte_source.h"
#include "las_error.h"
#include "HLas.h"
#include <cstdint>
#include <memory>
#include <string>

namespace HLas::detail {

// ByteSource över HTTP range requests.
//
// Det här är den enda platsen i biblioteket där ett externt beroende (libcurl)
// får förekomma, och modulen byggs bara med -DHLAS_ENABLE_NET=ON. core, copc,
// dem och manager vet inte att den finns — den som bara vill läsa lokala filer
// får aldrig se libcurl.
//
// Behöver du inte vår implementation går det lika bra att ärva ByteSource själv
// och koppla in en befintlig HTTP-klient. Interfacet är hela integrationspunkten.
//
// Trådsäkerhet: en instans äger ett CURL-handtag och är inte trådsäker. En
// instans per tråd gäller, precis som för Reader.
//
// SSRF: open() begränsar till http och https, även genom omdirigeringar, och
// följer högst fem hopp. Vilka värdar som är känsliga vet bara anroparen — se
// kommentaren vid CopcReader::open_url() i den publika headern. Att lägga en
// värdpolicy här vore fel: biblioteket har inget underlag för att avgöra vad
// som är internt i någon annans nät, och en gissning skulle antingen blockera
// legitima uppsättningar eller ge falsk trygghet.
class HttpByteSource final : public ByteSource {
public:
    HttpByteSource();
    ~HttpByteSource() override;

    HttpByteSource(const HttpByteSource&) = delete;
    HttpByteSource& operator=(const HttpByteSource&) = delete;

    // Kopplar upp mot url och tar reda på resursens storlek.
    //
    // Servern MÅSTE stödja range requests. Utan det skulle varje läsning hämta
    // hela filen, vilket är precis tvärtemot poängen — bättre att neka direkt än
    // att tyst bli katastrofalt långsam.
    // `username`/`password` för servrar som kräver HTTP Basic, som många
    // nedladdningsservrar gör. Skickas bara över https: Basic är base64,
    // alltså klartext, och att låta det gå över http vore att lämna ut
    // uppgifterna. Ett sådant försök nekas i stället för att lyckas tyst.
    // `settings` beskriver nätet där programmet körs — proxy, CA-källa,
    // timeouts. Ett förvalt objekt ger exakt det beteende biblioteket hade innan
    // inställningarna fanns.
    ErrorCode open(const char* url,
                   const char* username = nullptr,
                   const char* password = nullptr,
                   const NetworkSettings& settings = NetworkSettings{});
    void close();
    bool is_open() const { return total_ != 0 || opened_; }

    bool read(uint64_t offset, size_t len, uint8_t* dest) override;
    uint64_t size() const override { return total_; }

    // Ny uppkoppling mot samma URL. Kostar en handskakning (~600 ms mot S3),
    // men den betalas en gång per tråd och tjänas in på första noden.
    std::unique_ptr<ByteSource> clone() const override;

    // Mätvärden — hur mycket som faktiskt hämtats.
    uint64_t request_count() const { return requests_; }
    uint64_t bytes_fetched() const { return fetched_; }
    const std::string& last_error() const { return last_error_; }

private:
    void fail_close(std::string msg);

    void*       curl_ = nullptr;      // CURL*
    std::string url_;
    uint64_t    total_    = 0;
    bool        opened_   = false;
    uint64_t    requests_ = 0;
    uint64_t    fetched_  = 0;
    std::string last_error_;
    // Sparas för clone(). Uppgifterna ligger i klartext i minnet så länge
    // objektet lever, och kopieras till varje klon. std::string::clear()
    // garanterar inte att bytena skrivs över, så de kan ligga kvar i frigjord
    // allokering. Vill man undvika det får uppgifterna hämtas per anrop i
    // stället för att sparas — priset är att clone() inte kan återansluta.
    std::string user_, pass_;
    // Sparas av samma skäl som uppgifterna: clone() måste kunna göra en
    // LIKADAN uppkoppling. En klon som fick förvalen i stället hade felat mitt
    // under en hämtning, på en tråd, med ett fel som inte liknar det första.
    NetworkSettings settings_;
};

} // namespace HLas::detail

#endif // HLAS_HAVE_NET
