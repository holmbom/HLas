#ifdef HLAS_HAVE_NET
#include "http_byte_source.h"
#include "curl_plan.h"
#include <curl/curl.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace HLas::detail {

namespace {

// libcurl initierar sig själv lazy vid första curl_easy_init(), men det är
// dokumenterat som osäkert när flera trådar startar samtidigt. En instans per
// tråd är hela poängen med den här klassen, så vi gör det explicit och en gång.
// Ingen matchande curl_global_cleanup(): den är processglobal och får inte köras
// medan andra delar av programmet fortfarande använder curl.
void ensure_curl_global_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct WriteTarget {
    uint8_t* dest = nullptr;
    size_t   cap  = 0;
    size_t   got  = 0;
    bool     excess = false;   // servern skickade mer än vi bad om
};

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* t = static_cast<WriteTarget*>(userdata);
    const size_t incoming = size * nmemb;
    const size_t room = t->cap - t->got;      // got <= cap alltid
    const size_t take = incoming < room ? incoming : room;
    if (take)
        std::memcpy(t->dest + t->got, ptr, take);
    t->got += take;
    if (take < incoming)
        t->excess = true;
    // Returnera incoming så att curl inte avbryter med skrivfel — vi vill
    // upptäcka avvikelsen själva och kunna säga vad som var fel.
    return incoming;
}

size_t discard_cb(char*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

// Strikt tolkning av "bytes <first>-<last>/<total>".
//
// Slarvig tolkning är farlig här: fältet är hur vi vet att servern gav oss det
// intervall vi bad om. strtoull skulle svälja "+123", "-1" (som blir enormt)
// och skräp efter siffrorna.
bool parse_u64(const char*& p, uint64_t& out) {
    if (!std::isdigit(static_cast<unsigned char>(*p)))
        return false;
    uint64_t v = 0;
    while (std::isdigit(static_cast<unsigned char>(*p))) {
        const unsigned d = static_cast<unsigned>(*p - '0');
        if (v > (UINT64_MAX - d) / 10)
            return false;                      // spill
        v = v * 10 + d;
        ++p;
    }
    out = v;
    return true;
}

bool parse_content_range(const char* s, uint64_t& first, uint64_t& last, uint64_t& total) {
    if (!s) return false;
    while (*s == ' ') ++s;
    if (std::strncmp(s, "bytes", 5) != 0) return false;
    s += 5;
    if (*s != ' ') return false;
    while (*s == ' ') ++s;

    if (!parse_u64(s, first)) return false;
    if (*s++ != '-') return false;
    if (!parse_u64(s, last)) return false;
    if (*s++ != '/') return false;
    if (!parse_u64(s, total)) return false;    // "*" avvisas: okänd total duger inte
    while (*s == ' ') ++s;
    if (*s != '\0') return false;              // inget skräp efter

    return first <= last && last < total;
}

const char* header_value(CURL* c, const char* name) {
    curl_header* h = nullptr;
    if (curl_easy_header(c, name, 0, CURLH_HEADER, -1, &h) != CURLHE_OK || !h)
        return nullptr;
    return h->value;
}


// Spårningsutskrift för CURLOPT_VERBOSE.
//
// Ersätter libcurls egen, som skriver ALLT till stderr — inklusive
// Authorization-huvuden och, med tillräcklig nivå, kroppen. Punktdata i en
// terminallogg är inte bara oanvändbart utan gigabyte.
//
// KROPPEN kan inte läcka: alla DATA- och SSL_DATA-typer förkastas utan att ens
// granskas. Rader som nämner authorization, cookie eller proxy-user maskas,
// oavsett om de är huvudrader eller libcurls egen spårningstext.
//
// ⚠️ Den senare är ändå inte en garanti. CURLINFO_TEXT är libcurls fritext och
// kan i princip formulera sig på sätt namnlistan inte täcker. Spårningen är
// avsedd för felsökning, inte för en logg som sparas.
int debug_cb(void*, curl_infotype type, char* data, size_t size, void*) {
    const char* tag;
    switch (type) {
        case CURLINFO_TEXT:       tag = "*"; break;
        case CURLINFO_HEADER_OUT: tag = ">"; break;
        case CURLINFO_HEADER_IN:  tag = "<"; break;
        // DATA_IN, DATA_OUT, SSL_DATA_IN, SSL_DATA_OUT — kroppen. Aldrig.
        default: return 0;
    }

    // Ett anrop kan bära flera rader; de granskas var för sig, annars maskar en
    // känslig rad bort de oskyldiga runt sig eller tvärtom.
    size_t start = 0;
    while (start < size) {
        size_t end = start;
        while (end < size && data[end] != '\n') ++end;
        size_t len = end - start;
        if (len && data[start + len - 1] == '\r') --len;

        if (len) {
            if (is_sensitive_line(data + start, len)) {
                // Bara namnet före kolon behålls. Saknas kolon — vilket
                // spårningstext ofta gör — maskas hela raden.
                const char* colon = static_cast<const char*>(
                    std::memchr(data + start, ':', len));
                const int name_len = colon ? static_cast<int>(colon - (data + start)) : 0;
                std::fprintf(stderr, "%s %.*s<maskerad>\n", tag, name_len, data + start);
            } else {
                std::fprintf(stderr, "%s %.*s\n", tag, static_cast<int>(len), data + start);
            }
        }
        start = (end < size) ? end + 1 : size;
    }
    return 0;
}

// Sätter anroparens inställningar på handtaget.
//
// Vad som ska sättas avgörs av make_curl_plan() i curl_plan.h — den här
// funktionen översätter bara beslutet till anrop. Uppdelningen finns för att
// besluten är det som går fel, och som ett värde går de att pröva utan nät.
//
// Alla optioner som används här är äldre än CURLOPT_PROTOCOLS_STR, som redan
// krävs på annat håll i filen — därför behövs inga versionsvillkor.
void apply_settings(CURL* c, const NetworkSettings& s) {
    const CurlPlan p = make_curl_plan(s);

    if (p.set_proxy) {
        curl_easy_setopt(c, CURLOPT_PROXY, p.proxy_value.c_str());
        if (p.set_proxy_credentials) {
            curl_easy_setopt(c, CURLOPT_PROXYUSERNAME, s.proxy_user.c_str());
            curl_easy_setopt(c, CURLOPT_PROXYPASSWORD, s.proxy_password.c_str());
        }
    }

    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, p.verify_peer);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, p.verify_host);
    if (p.set_ca_file)
        curl_easy_setopt(c, CURLOPT_CAINFO, s.ca_file.c_str());
    if (p.native_ca)
        curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);

    // Proxyns TLS sätts UTTRYCKLIGEN, även när verifieringen är av. Att hoppa
    // över anropen hade lämnat libcurls förval 1/2 kvar, och proxy-benet hade
    // fortsatt verifiera trots att anroparen bett om motsatsen.
    if (p.set_proxy_tls) {
        curl_easy_setopt(c, CURLOPT_PROXY_SSL_VERIFYPEER, p.proxy_verify_peer);
        curl_easy_setopt(c, CURLOPT_PROXY_SSL_VERIFYHOST, p.proxy_verify_host);
        if (p.set_proxy_ca_file)
            curl_easy_setopt(c, CURLOPT_PROXY_CAINFO, s.ca_file.c_str());
        if (p.proxy_native_ca)
            curl_easy_setopt(c, CURLOPT_PROXY_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
    }

    if (p.http_version == 1)
        curl_easy_setopt(c, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    else if (p.http_version == 2)
        curl_easy_setopt(c, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2_0);

    // Sätts ALLTID, med nullptr när den är av. Se CurlPlan::compression.
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING,
                     p.compression ? "gzip, deflate" : nullptr);

    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, p.connect_timeout);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        p.transfer_timeout);

    if (p.set_user_agent)
        curl_easy_setopt(c, CURLOPT_USERAGENT, s.user_agent.c_str());

    if (p.verbose) {
        curl_easy_setopt(c, CURLOPT_DEBUGFUNCTION, debug_cb);
        curl_easy_setopt(c, CURLOPT_VERBOSE, 1L);
    }
}

} // namespace

HttpByteSource::HttpByteSource() = default;

HttpByteSource::~HttpByteSource() {
    close();
}

void HttpByteSource::close() {
    if (curl_) {
        curl_easy_cleanup(static_cast<CURL*>(curl_));
        curl_ = nullptr;
    }
    url_.clear();
    user_.clear();
    pass_.clear();
    // Inställningarna nollställs med resten. En återanvänd källa ska inte bära
    // med sig föregående uppkopplings nät.
    settings_ = NetworkSettings{};
    total_ = 0;
    opened_ = false;
    requests_ = 0;
    fetched_ = 0;
    last_error_.clear();
}

// Stänger men behåller felmeddelandet — annars förlorar varje misslyckad open()
// sin diagnostik i städningen och last_error() blir tom just när den behövs.
void HttpByteSource::fail_close(std::string msg) {
    close();
    last_error_ = std::move(msg);
}

ErrorCode HttpByteSource::open(const char* url,
                               const char* username, const char* password,
                               const NetworkSettings& settings) {
    close();
    if (!url || !*url) {
        last_error_ = "empty URL";
        return ErrorCode::FileNotFound;
    }

    const bool want_auth = username && *username;
    if (want_auth && std::strncmp(url, "https://", 8) != 0) {
        last_error_ = "refusing to send credentials over a non-https URL";
        return ErrorCode::FileOpenFailed;
    }

    ensure_curl_global_init();

    CURL* c = curl_easy_init();
    if (!c) {
        last_error_ = "curl_easy_init failed";
        return ErrorCode::FileOpenFailed;
    }
    curl_ = c;
    url_ = url;
    user_ = username ? username : "";
    pass_ = password ? password : "";
    settings_ = settings;

    curl_easy_setopt(c, CURLOPT_URL, url_.c_str());
    // Bara HTTP(S), både för den första förfrågan och för omdirigeringar. Utan
    // det kan en URL peka på file://, scp:// eller ftp:// och libcurl gör det
    // glatt. Vilka *värdar* som är tillåtna är anroparens sak — biblioteket kan
    // inte veta vad som är internt i deras nät.
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);

    // Anroparens inställningar sätts sist. De rör INTE protokollbegränsningen,
    // MAXREDIRS eller UNRESTRICTED_AUTH — de tre är säkerhetsval biblioteket gör
    // själv och som inte går att ställa in. Timeouts sätts härinne och inte ovan,
    // eftersom de skalas.
    apply_settings(c, settings_);
    if (want_auth) {
        curl_easy_setopt(c, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(c, CURLOPT_USERNAME, username);
        curl_easy_setopt(c, CURLOPT_PASSWORD, password ? password : "");
        // Följ inte med uppgifterna till en annan värd vid omdirigering.
        curl_easy_setopt(c, CURLOPT_UNRESTRICTED_AUTH, 0L);
    }

    curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, nullptr);

    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        fail_close(curl_easy_strerror(rc));
        return ErrorCode::FileOpenFailed;
    }

    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (status != 206) {
        char buf[128];
        std::snprintf(buf, sizeof buf,
                      "server ignored Range (HTTP %ld, expected 206)", status);
        fail_close(buf);
        return ErrorCode::FileOpenFailed;
    }

    // Content-Range ger både resursens totala storlek och beviset att servern
    // svarade på det intervall vi bad om.
    uint64_t first = 0, last = 0, total = 0;
    if (!parse_content_range(header_value(c, "Content-Range"), first, last, total)) {
        fail_close("missing or malformed Content-Range");
        return ErrorCode::FileOpenFailed;
    }
    if (first != 0 || last != 0) {
        fail_close("server returned a different range than requested");
        return ErrorCode::FileOpenFailed;
    }

    total_ = total;
    opened_ = true;
    return ErrorCode::Ok;
}

bool HttpByteSource::read(uint64_t offset, size_t len, uint8_t* dest) {
    if (!curl_ || !opened_ || !dest)
        return false;
    if (!range_ok(offset, len, total_))
        return false;
    if (len == 0)
        return true;

    const uint64_t want_first = offset;
    const uint64_t want_last  = offset + len - 1;

    char range[64];
    std::snprintf(range, sizeof range, "%llu-%llu",
                  static_cast<unsigned long long>(want_first),
                  static_cast<unsigned long long>(want_last));

    WriteTarget t{ dest, len, 0, false };
    CURL* c = static_cast<CURL*>(curl_);

    // Samma handtag återanvänds, så uppkopplingen och TLS-sessionen hålls vid
    // liv mellan noder. Mätt: första förfrågan 924 ms, följande ~140 ms.
    curl_easy_setopt(c, CURLOPT_RANGE, range);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &t);

    CURLcode rc = curl_easy_perform(c);
    ++requests_;
    curl_easy_setopt(c, CURLOPT_WRITEDATA, nullptr);   // t dör när vi återvänder

    if (rc != CURLE_OK) {
        last_error_ = curl_easy_strerror(rc);
        return false;
    }

    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (status != 206) {
        last_error_ = "server ignored Range";
        return false;
    }

    // Statuskoden räcker inte. En trasig eller elak server kan svara 206 med
    // ett annat intervall än det begärda, och då hade vi tolkat fel byte som
    // punktdata utan att märka något.
    uint64_t got_first = 0, got_last = 0, got_total = 0;
    if (!parse_content_range(header_value(c, "Content-Range"),
                             got_first, got_last, got_total)) {
        last_error_ = "missing or malformed Content-Range";
        return false;
    }
    if (got_first != want_first || got_last != want_last || got_total != total_) {
        last_error_ = "server returned a different range than requested";
        return false;
    }
    if (t.excess) {
        last_error_ = "server sent more data than the requested range";
        return false;
    }
    if (t.got != len) {
        last_error_ = "short read";
        return false;
    }

    fetched_ += len;
    return true;
}

std::unique_ptr<ByteSource> HttpByteSource::clone() const {
    if (!opened_ || url_.empty())
        return nullptr;
    auto c = std::make_unique<HttpByteSource>();
    // Inställningarna MÅSTE med. En klon som fick förvalen hade kopplat upp
    // annorlunda än originalet — och felet kommit mitt under en hämtning, på en
    // annan tråd, med ett fel som inte liknar det som fungerade vid open().
    if (c->open(url_.c_str(),
                user_.empty() ? nullptr : user_.c_str(),
                pass_.empty() ? nullptr : pass_.c_str(),
                settings_) != ErrorCode::Ok)
        return nullptr;
    return c;
}

} // namespace HLas::detail

#endif // HLAS_HAVE_NET
