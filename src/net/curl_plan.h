#pragma once
#ifdef HLAS_HAVE_NET

#include "HLas.h"
#include <cctype>
#include <cstring>
#include <string>

namespace HLas::detail {

// Vad anroparens NetworkSettings BETYDER, skilt från att sätta det på ett
// handtag.
//
// Uppdelningen finns för att fällorna ligger i BESLUTEN, inte i anropen. Att
// sätta en option är mekaniskt; att veta att "ingen proxy" och "systemets proxy"
// kräver motsatta anrop är det inte. Med beslutet som ett värde går det att
// pröva utan nät, utan libcurl och utan en server — och de fyra fällorna nedan
// blir testfall i stället för kommentarer.
struct CurlPlan {
    // ---- Proxy ----
    // ⛔ set_proxy skiljer System från de andra två. System sätter INGENTING,
    // och det är frånvaron som får libcurl att läsa http_proxy/https_proxy.
    // None sätter tom sträng, vilket stänger av proxyn OCH ignorerar miljön.
    bool        set_proxy = false;
    std::string proxy_value;              // "" för None, URL:en för Manual
    bool        set_proxy_credentials = false;

    // ---- TLS mot servern ----
    // De två följs åt: 1/2 eller 0/0. Aldrig 1 i verify_host — det värdet är
    // borttaget ur libcurl och ger fel i stället för svagare kontroll.
    long        verify_peer = 1;
    long        verify_host = 2;
    bool        set_ca_file = false;      // ⛔ falskt när ca_file är tom
    bool        native_ca = false;

    // ---- TLS mot proxyn, HÄRLETT ----
    //
    // ⛔ set_proxy_tls betyder "vi bestämmer över proxyns TLS", inte "verifiera
    // proxyn". Värdena ligger i proxy_verify_*. Skillnaden är inte akademisk:
    // hoppar man över anropen när verifieringen är AV blir libcurls förval 1/2
    // kvar, och proxy-benet fortsätter verifiera trots att anroparen bett om
    // motsatsen. Det är exakt det fel hela den här inställningen finns för att
    // rätta, fast ett steg längre ut i kedjan.
    bool        set_proxy_tls = false;
    long        proxy_verify_peer = 1;
    long        proxy_verify_host = 2;
    bool        set_proxy_ca_file = false;
    bool        proxy_native_ca = false;

    // ---- Övrigt ----
    int         http_version = 0;         // 0 = sätt inget, 1 = 1.1, 2 = 2.0
    // ⛔ Komprimeringen sätts ALLTID, med nullptr när den är av. Att hoppa över
    // anropet är inte samma sak som att stänga av den på ett handtag som
    // återanvänds.
    bool        compression = false;
    long        connect_timeout = 15;
    long        transfer_timeout = 60;
    bool        set_user_agent = false;
    bool        verbose = false;
};

// Skalar en timeout med anroparens procenttal.
//
// Ett värde <= 0 är meningslöst som procent och tolkas som 100 — men noll är
// dessutom FARLIGT att släppa igenom, eftersom noll i libcurl betyder "ingen
// gräns". En hängd uppkoppling hade då aldrig återvänt. Resultatet klämms också
// nedåt till en sekund av samma skäl, och uppåt så att multiplikationen inte
// kan spilla över.
inline long scale_timeout(long base, int percent) {
    if (percent <= 0) percent = 100;
    if (percent > 100000) percent = 100000;
    const long long v = (static_cast<long long>(base) * percent) / 100;
    return v < 1 ? 1L : static_cast<long>(v);
}

inline CurlPlan make_curl_plan(const NetworkSettings& s) {
    CurlPlan p;

    switch (s.proxy_mode) {
        case NetworkSettings::ProxyMode::None:
            p.set_proxy = true;
            p.proxy_value.clear();
            break;
        case NetworkSettings::ProxyMode::System:
            p.set_proxy = false;
            break;
        case NetworkSettings::ProxyMode::Manual:
            p.set_proxy = true;
            p.proxy_value = s.proxy_url;
            p.set_proxy_credentials = !s.proxy_user.empty();
            break;
    }

    p.verify_peer = s.verify_peer ? 1L : 0L;
    p.verify_host = s.verify_peer ? 2L : 0L;
    p.set_ca_file = !s.ca_file.empty();
    p.native_ca   = s.use_native_ca;

    // Proxyns TLS härleds ur serverns, HELA vägen: verifieringen, CA-filen och
    // plattformens CA-lager. Förenklingen antar SAMMA CA-källa för båda, vilket
    // stämmer på ett nät med TLS-inspektion där de är samma företagsrot.
    //
    // Det spelar bara roll när proxy-URL:en SJÄLV är https — en vanlig
    // http-proxy gör CONNECT, och då är TLS:et mot servern.
    p.set_proxy_tls     = (s.proxy_mode == NetworkSettings::ProxyMode::Manual);
    p.proxy_verify_peer = p.verify_peer;
    p.proxy_verify_host = p.verify_host;
    p.set_proxy_ca_file = p.set_proxy_tls && s.verify_peer && p.set_ca_file;
    p.proxy_native_ca   = p.set_proxy_tls && s.verify_peer && p.native_ca;

    switch (s.http_version) {
        case NetworkSettings::HttpVersion::Http1_1: p.http_version = 1; break;
        case NetworkSettings::HttpVersion::Http2:   p.http_version = 2; break;
        case NetworkSettings::HttpVersion::Default: p.http_version = 0; break;
    }

    p.compression       = s.compression;
    p.connect_timeout   = scale_timeout(15, s.timeout_scale);
    p.transfer_timeout  = scale_timeout(60, s.timeout_scale);
    p.set_user_agent    = !s.user_agent.empty();
    p.verbose           = s.verbose;
    return p;
}

// Skiljer huvudrader som inte får hamna i en logg.
//
// Ligger här och inte hos spårningsfunktionen av samma skäl som planen: det är
// ett BESLUT, det är säkerhetsrelevant, och som fri funktion går det att pröva.
//
// Jämförelsen är skiftlägesokänslig och letar var som helst i raden, inte bara i
// början. Huvudrader börjar visserligen med namnet, men libcurls egen
// spårningstext gör det inte — och den kan nämna samma saker.
inline bool is_sensitive_line(const char* line, size_t len) {
    static const char* kNames[] = {
        "authorization", "cookie", "proxy-user", "set-cookie",
    };
    for (const char* name : kNames) {
        const size_t n = std::strlen(name);
        if (len < n) continue;
        for (size_t start = 0; start + n <= len; ++start) {
            size_t i = 0;
            for (; i < n; ++i) {
                const char a = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(line[start + i])));
                if (a != name[i]) break;
            }
            if (i == n) return true;
        }
    }
    return false;
}

} // namespace HLas::detail

#endif // HLAS_HAVE_NET
