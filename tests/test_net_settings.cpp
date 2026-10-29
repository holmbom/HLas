// Vad NetworkSettings BETYDER, prövat utan nät.
//
// Fällorna i det här API:t ligger i besluten, inte i anropen: att "ingen proxy"
// och "systemets proxy" kräver MOTSATTA anrop, att en tom CA-sökväg betyder
// "sätt ingenting" och inte "leta rätt på en", att namnkontrollen måste följa
// kedjekontrollen, och att komprimering av inte är samma sak som att hoppa över
// anropet. Var och en av dem är lätt att skriva fel och omöjlig att se på en fil
// som ändå laddas ner.
//
// Därför ligger besluten i make_curl_plan() som ett VÄRDE, och det värdet går
// att granska utan server, utan libcurl-handtag och utan att någon byte skickas.

#include "HLas.h"
#ifdef HLAS_HAVE_NET
#include "curl_plan.h"
#endif

#include <cstdio>
#include <cstring>

#define CHECK(expr) do { if (!(expr)) { printf("  FAIL: %s (line %d)\n", #expr, __LINE__); ++fails; } } while(0)

int test_net_settings() {
    int fails = 0;
    printf("test_net_settings:\n");

    using NS = HLas::NetworkSettings;

    // Förvalen ska återge exakt det beteende biblioteket hade INNAN
    // inställningarna fanns. Den som inte bryr sig ska inte märka att de finns.
    {
        const NS d;
        CHECK(d.proxy_mode == NS::ProxyMode::System);
        CHECK(d.verify_peer == true);
        CHECK(d.use_native_ca == false);
        CHECK(d.http_version == NS::HttpVersion::Default);
        CHECK(d.compression == false);
        CHECK(d.timeout_scale == 100);
        CHECK(d.verbose == false);
        CHECK(d.ca_file.empty() && d.proxy_url.empty() && d.user_agent.empty());
    }

    // Settern och getarna finns ÄVEN utan nät, och lagrar. Annars gick inte ett
    // program att kompilera mot ett bibliotek utan nätstöd, och det vore ett
    // byggfel för något som ska vara ett körningsfel.
    {
        HLas::CopcReader r;
        NS s;
        s.proxy_mode = NS::ProxyMode::Manual;
        s.proxy_url = "http://proxy.example:3128";
        s.ca_file = "/etc/ssl/företag.pem";
        s.verify_peer = false;
        s.timeout_scale = 250;
        s.user_agent = "HLasTest/2.0";
        r.set_network_settings(s);

        const NS& back = r.network_settings();
        CHECK(back.proxy_mode == NS::ProxyMode::Manual);
        CHECK(back.proxy_url == "http://proxy.example:3128");
        CHECK(back.ca_file == "/etc/ssl/företag.pem");
        CHECK(back.verify_peer == false);
        CHECK(back.timeout_scale == 250);
        CHECK(back.user_agent == "HLasTest/2.0");
    }

#ifndef HLAS_HAVE_NET
    printf("  OK — förval och setter (byggt utan nät, planen prövas inte)\n");
#else
    using HLas::detail::make_curl_plan;
    using HLas::detail::scale_timeout;

    // ---- Proxyns tre lägen, som INTE är grader av samma sak ----
    {
        NS s;
        s.proxy_mode = NS::ProxyMode::System;
        const auto p = make_curl_plan(s);
        // Systemets proxy = sätt INGENTING. Det är frånvaron som får libcurl
        // att läsa http_proxy/https_proxy.
        CHECK(p.set_proxy == false);
    }
    {
        NS s;
        s.proxy_mode = NS::ProxyMode::None;
        const auto p = make_curl_plan(s);
        // Ingen proxy = sätt TOM STRÄNG. Det stänger av proxyn och gör att
        // miljövariablerna ignoreras — motsatsen till System, inte en variant.
        CHECK(p.set_proxy == true);
        CHECK(p.proxy_value.empty());
        CHECK(p.set_proxy_credentials == false);
    }
    {
        NS s;
        s.proxy_mode = NS::ProxyMode::Manual;
        s.proxy_url = "http://proxy.example:3128";
        const auto p = make_curl_plan(s);
        CHECK(p.set_proxy == true);
        CHECK(p.proxy_value == "http://proxy.example:3128");
        CHECK(p.set_proxy_credentials == false);      // inget användarnamn satt
        s.proxy_user = "anv";
        CHECK(make_curl_plan(s).set_proxy_credentials == true);
    }

    // ---- Namnkontrollen följer kedjekontrollen ----
    {
        NS s;
        s.verify_peer = true;
        auto p = make_curl_plan(s);
        CHECK(p.verify_peer == 1 && p.verify_host == 2);   // aldrig 1 i verify_host

        s.verify_peer = false;
        p = make_curl_plan(s);
        // Båda av. Vore verify_host kvar på 2 skulle namnet fortfarande
        // kontrolleras mot certifikatet, och på ett nät med TLS-inspektion är det
        // just den kontrollen som fäller anslutningen — "stäng av verifieringen"
        // hade då inte gjort det anroparen bad om.
        CHECK(p.verify_peer == 0 && p.verify_host == 0);
    }

    // ---- Tom CA-sökväg betyder "sätt ingenting" ----
    {
        NS s;
        CHECK(make_curl_plan(s).set_ca_file == false);
        s.ca_file = "/etc/ssl/certs/ca-bundle.crt";
        CHECK(make_curl_plan(s).set_ca_file == true);
    }

    // ---- Plattformens CA-lager är anroparens beslut ----
    {
        NS s;
        CHECK(make_curl_plan(s).native_ca == false);
        s.use_native_ca = true;
        CHECK(make_curl_plan(s).native_ca == true);
    }

    // ---- Proxyns TLS härleds ur serverns ----
    {
        NS s;
        s.verify_peer = true;
        // System- och None-läge har ingen proxy att bestämma över.
        s.proxy_mode = NS::ProxyMode::System;
        CHECK(make_curl_plan(s).set_proxy_tls == false);
        s.proxy_mode = NS::ProxyMode::None;
        CHECK(make_curl_plan(s).set_proxy_tls == false);

        s.proxy_mode = NS::ProxyMode::Manual;
        s.proxy_url = "https://proxy.example:3128";
        auto p = make_curl_plan(s);
        CHECK(p.set_proxy_tls == true);
        CHECK(p.proxy_verify_peer == 1 && p.proxy_verify_host == 2);
        CHECK(p.set_proxy_ca_file == false);            // ingen CA-fil satt
        CHECK(p.proxy_native_ca == false);

        s.ca_file = "/etc/ssl/företag.pem";
        s.use_native_ca = true;
        p = make_curl_plan(s);
        CHECK(p.set_proxy_ca_file == true);             // samma fil som origin
        CHECK(p.proxy_native_ca == true);               // samma lager som origin

        // AVSTÄNGD VERIFIERING MÅSTE NÅ PROXYN OCKSÅ.
        //
        // set_proxy_tls ska stå kvar SANN med 0/0 — hoppar man över anropen
        // blir libcurls förval 1/2 kvar, och proxy-benet fortsätter verifiera
        // trots att anroparen bett om motsatsen. Det är samma fel som hela den
        // här inställningen finns för att rätta, ett steg längre ut i kedjan.
        s.verify_peer = false;
        p = make_curl_plan(s);
        CHECK(p.set_proxy_tls == true);
        CHECK(p.proxy_verify_peer == 0 && p.proxy_verify_host == 0);
        CHECK(p.set_proxy_ca_file == false);
        CHECK(p.proxy_native_ca == false);
    }

    // ---- Maskningen i spårningen ----
    {
        using HLas::detail::is_sensitive_line;
        auto sens = [](const char* s) {
            return is_sensitive_line(s, std::strlen(s));
        };
        CHECK(sens("Authorization: Basic YWJjOjEyMw=="));
        CHECK(sens("authorization: Bearer x"));            // skiftlägesokänsligt
        CHECK(sens("Proxy-Authorization: Basic x"));
        CHECK(sens("Cookie: session=hemlig"));
        CHECK(sens("Set-Cookie: session=hemlig"));
        // Libcurls egen spårningstext börjar INTE med namnet, så jämförelsen
        // måste leta var som helst i raden.
        CHECK(sens("* Server auth using Basic with Authorization"));
        // Och de oskyldiga raderna ska INTE maskas, annars blir spårningen
        // oanvändbar för det den finns till för.
        CHECK(!sens("< HTTP/1.1 206 Partial Content"));
        CHECK(!sens("< Content-Range: bytes 0-0/81123042"));
        CHECK(!sens("* Trying 1.2.3.4:443..."));
        CHECK(!sens(""));
    }

    // ---- HTTP-version ----
    {
        NS s;
        CHECK(make_curl_plan(s).http_version == 0);   // förval: sätt inget
        s.http_version = NS::HttpVersion::Http1_1;
        CHECK(make_curl_plan(s).http_version == 1);
        s.http_version = NS::HttpVersion::Http2;
        CHECK(make_curl_plan(s).http_version == 2);
    }

    // ---- Timeouts skalas, och BÅDA två ----
    {
        NS s;
        auto p = make_curl_plan(s);
        CHECK(p.connect_timeout == 15 && p.transfer_timeout == 60);

        s.timeout_scale = 300;
        p = make_curl_plan(s);
        // Den senare är den som failar först på en långsam förbindelse, mitt i
        // en hämtning — skalan måste nå den och inte bara uppkopplingen.
        CHECK(p.connect_timeout == 45 && p.transfer_timeout == 180);
    }

    // Noll och negativa procenttal. NOLL ÄR FARLIGT att släppa igenom: i libcurl
    // betyder noll "ingen gräns", så en hängd uppkoppling hade aldrig återvänt.
    {
        CHECK(scale_timeout(60, 0)    == 60);     // tolkas som 100
        CHECK(scale_timeout(60, -5)   == 60);
        CHECK(scale_timeout(60, 1)    == 1);      // klämd nedåt, aldrig 0
        CHECK(scale_timeout(15, 1)    == 1);
        CHECK(scale_timeout(60, 100)  == 60);
        // Taket finns för att multiplikationen inte ska kunna spilla över.
        CHECK(scale_timeout(60, 1000000) == scale_timeout(60, 100000));
        CHECK(scale_timeout(60, 100000) > 0);
    }

    // ---- Komprimering ----
    {
        NS s;
        CHECK(make_curl_plan(s).compression == false);
        s.compression = true;
        CHECK(make_curl_plan(s).compression == true);
    }

    // ---- User-Agent och spårning ----
    {
        NS s;
        CHECK(make_curl_plan(s).set_user_agent == false);
        CHECK(make_curl_plan(s).verbose == false);
        s.user_agent = "HLasTest/2.0";
        s.verbose = true;
        CHECK(make_curl_plan(s).set_user_agent == true);
        CHECK(make_curl_plan(s).verbose == true);
    }

    // Ett förvalt objekt ska ge den plan som motsvarar bibliotekets beteende
    // före 1.1.2: ingen proxy-option, verifiering på, inga CA-val, inga
    // versionsval, 15 och 60 sekunder.
    {
        const auto p = make_curl_plan(NS{});
        CHECK(!p.set_proxy && !p.set_ca_file && !p.native_ca && !p.set_proxy_tls);
        CHECK(p.verify_peer == 1 && p.verify_host == 2);
        CHECK(p.http_version == 0 && !p.compression && !p.set_user_agent && !p.verbose);
        CHECK(p.connect_timeout == 15 && p.transfer_timeout == 60);
    }

    if (fails == 0)
        printf("  OK — planen för proxyns tre lägen, verify-paret, härledd"
               " proxy-TLS, skalade timeouts och maskningen\n");
#endif
    return fails;
}
