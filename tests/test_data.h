// Central plats för testdata. Byt filer här — inte i tolv testfiler.
//
// Filerna ligger inte i repot (las/ är gitignorerad). Hämta dem med:
//
//   mkdir -p las
//   curl -o las/autzen.laz                    https://s3.amazonaws.com/hobu-lidar/autzen.laz
//   curl -o las/autzen-classified.laz         https://s3.amazonaws.com/hobu-lidar/autzen-classified.laz
//   curl -o las/autzen-classified.copc.laz    https://s3.amazonaws.com/hobu-lidar/autzen-classified.copc.laz
//
// las/autzen.las är okomprimerad och genereras ur autzen.laz — det finns ingen
// publik .las att hämta. Se tools/make_las.cpp.
//
// Saknas en fil hoppas testet över i stället för att krascha (SKIP_IF_MISSING).

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

// För utf8_to_wide/wide_to_utf8/remove_utf8 nedan. Headern definierar NOMINMAX
// innan den drar in windows.h, så std::min och std::max överlever den här
// includen — vilket de inte gjorde före 1.1.4:s Windows-rättelse.
#include "path_utf8.h"

namespace testdata {

// Katalogen för testernas EGNA temporärfiler — inte för testdatan.
//
// TMPDIR är en POSIX-konvention och sätts inte på Windows; där heter
// variablerna TEMP och TMP. Testerna föll tidigare tillbaka på "/tmp", som inte
// finns på Windows, och varje skrivning dit misslyckades. Symptomet var
// vilseledande: `LasWriter::create()` gav FileOpenFailed och testet rapporterade
// det som ett fel i SKRIVAREN, fast ingen katalog fanns att skriva i.
//
// På POSIX läses BARA TMPDIR, precis som förr. På Windows provas TMPDIR först
// och sedan TEMP och TMP, så att en körning som sätter TMPDIR beter sig lika på
// båda plattformarna.
inline std::string tmp_dir() {
    std::string s;
#ifdef _WIN32
    // ⚠️ _wgetenv, inte getenv. getenv ger byten i den aktiva ANSI-kodsidan,
    // och ett TEMP under `C:\Users\Åsa\` är då inte giltig UTF-8 — alltså
    // avvisat av precis den konvertering biblioteket gör vid varje filöppning.
    // Testerna hade då felat på en maskin vars användarnamn inte är ren ASCII,
    // vilket för en svensk användare är det normala fallet.
    for (const wchar_t* var : {L"TMPDIR", L"TEMP", L"TMP"}) {
        const wchar_t* d = _wgetenv(var);
        if (!d || !*d) continue;
        s = HLas::detail::wide_to_utf8(d);
        if (!s.empty()) break;
    }
    if (s.empty()) s = ".";   // TEMP saknas i praktiken aldrig; bygg-katalogen duger
#else
    // Bara TMPDIR, inte TEMP/TMP: de senare är Windows-konventioner, och att
    // låta dem gälla här hade ändrat vilken katalog en Linux-körning väljer.
    // Den här grenen ska bete sig exakt som före Windows-rättelsen.
    if (const char* d = std::getenv("TMPDIR")) { if (*d) s = d; }
    if (s.empty()) s = "/tmp";
#endif
    // Bort med avslutande separator: anroparen lägger på sin egen, och en TEMP
    // som slutar på bakstreck ger annars två i rad.
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\'))
        s.pop_back();
    return s;
}

// ---------------------------------------------------------------------------
// Filoperationer som tål en UTF-8-sökväg.
//
// Testerna skriver till TEMP, och på en svensk maskin heter den ofta något i
// stil med `C:\Users\Åsa\AppData\Local\Temp`. std::fopen, std::remove och
// std::ifstream går på Windows via A-varianterna och tolkar byten i
// ANSI-kodsidan, så en sådan sökväg ger FileNotFound — samma fälla som
// biblioteket självt undviker via src/core/path_utf8.h. Testerna använder de
// här i stället, så att det som provas är biblioteket och inte testets egen
// filhantering.

inline std::FILE* fopen_utf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    const std::wstring w = HLas::detail::utf8_to_wide(path);
    if (w.empty()) return nullptr;
    const std::wstring wm(mode, mode + std::strlen(mode));   // lägen är ren ASCII
    return _wfopen(w.c_str(), wm.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

inline bool remove_utf8(const std::string& path) {
    return HLas::detail::remove_utf8(path);
}

// ifstream med en wchar_t*-sökväg är en utökning som både MSVC och libstdc++ på
// MinGW har, och det är enda vägen dit utan att gå via ett eget filhandtag.
// ⛔ const wchar_t*, inte std::wstring: överlagringen med std::wstring finns
// bara i MSVC, och MSYS2-bygget av sviten kompilerade inte (2026-10-20). Strömmen
// returneras per värde — den är flyttbar sedan C++11.
inline std::ifstream ifstream_utf8(const std::string& path,
                                   std::ios::openmode mode = std::ios::binary) {
#ifdef _WIN32
    const std::wstring w = HLas::detail::utf8_to_wide(path);
    if (w.empty()) return std::ifstream();
    return std::ifstream(w.c_str(), mode);
#else
    return std::ifstream(path, mode);
#endif
}

// Snedstrecket framåt fungerar även på Windows — både CRT:n och Win32 tar det.
inline std::string tmp_path(const std::string& name) {
    return tmp_dir() + "/" + name;
}

// LAS 1.2, format 3 (XYZ + GPS + RGB), okomprimerad, 34 B/pt — rå LAS-stig
inline constexpr const char* kLas = "las/autzen.las";

// LAZ 1.2, format 3, Chunked (Point10 + GPSTime11 + RGB12) — samma punkter som kLas,
// i samma ordning, vilket gör korsjämförelse LAS/LAZ meningsfull
inline constexpr const char* kLaz = "las/autzen.laz";

// COPC LAZ 1.4, format 7, LayerChunked (Point14 + RGB14)
inline constexpr const char* kCopc = "las/autzen-classified.copc.laz";

// COPC LAZ 1.4, format 7 — samma område, annan klassningskörning
inline constexpr const char* kCopcAlt = "las/autzen-classified.laz";

// --- Fakta som gäller alla fyra filerna (samma punktmoln, Autzen Stadium) ---
inline constexpr uint64_t kPointCount = 10653336;

inline constexpr double kMinX = 635577.79, kMaxX = 639003.73;
inline constexpr double kMinY = 848882.15, kMaxY = 853537.66;
inline constexpr double kMinZ = 406.14,    kMaxZ = 615.26;
inline constexpr double kScale = 0.01;

// points_by_return, retur 1–4 (summerar till kPointCount)
inline constexpr uint64_t kByReturn[4] = { 9100899, 1238442, 283980, 30015 };

// --- Punktlayout ---
inline constexpr uint16_t kRecLen12 = 34;  // LAS 1.2 format 3
inline constexpr uint16_t kRecLen14 = 36;  // LAS 1.4 format 7
inline constexpr uint16_t kRgbOff12 = 28;  // fmt 3: 20 + GPS(8)
inline constexpr uint16_t kRgbOff14 = 30;  // fmt 7: 22 + GPS(8)

// --- DEM ---
// Full utbredning är 3426 x 4656 m. 1 m-celler ger ~16M celler, vilket är rimligt
// för ren binning men alldeles för mycket för IDW/TIN — därför grövre celler där.
inline constexpr double kDemCell     = 1.0;
inline constexpr double kDemCellFill = 5.0;

// --- Fjärr-testmål för Fas 15 (COPC över HTTP) ---
//
// Samma fil som kCopc, men serverad av S3 med range request-stöd. Ingen
// uppladdning behövs — bucketen svarar `206 Partial Content` med
// `Accept-Ranges: bytes`, vilket är precis vad partiell hämtning kräver.
//
// Verifierat 2026-08-26 med två range requests på under en sekund: hela
// LAS-headern, alla tre VLR:erna och hierarkins placering gick att läsa utan
// att hämta de 78 MB:en. Värdena nedan är avlästa ur den hämtningen och duger
// som facit när net/ och copc/ ska testas.
inline constexpr const char* kCopcUrl =
    "https://s3.amazonaws.com/hobu-lidar/autzen-classified.copc.laz";

// Tre mätpunkter för att skilja bibliotekets beteende från nätets. Att bara mäta
// mot S3 blandar ihop transatlantisk latens med bibliotekets kostnader.
//
//   lokal webbserver  RTT 0,045 ms
//   VPS (Sverige)     RTT 24,9 ms
//   S3 (USA)          RTT 164 ms
//
// Uppmätt för en fråga som väljer 20 av 278 noder (6,07 MB, 237101 punkter):
//   källa            index    hämtning   uppackning
//   lokal fil          0 ms       1 ms       741 ms
//   lokal webbserver   1 ms      13 ms       755 ms
//   VPS (SE)          72 ms    1314 ms       743 ms
//   S3 (US)          616 ms    9835 ms       742 ms
//
// Uppackningen är konstant — den är CPU. Hämtningen skalar med rundturen, och
// brytpunkten ligger runt VPS:en. En trådpool som gör både hämtning och
// uppackning per nod vinner därför i båda regimerna.

inline constexpr uint64_t kCopcFileSize = 81123042;

// COPC info-VLR: user_id "copc", record_id 1, 160 byte, ligger först av tre.
inline constexpr uint16_t kCopcInfoRecordId = 1;
inline constexpr uint16_t kCopcInfoSize     = 160;

// Innehållet i copc info-VLR:n.
inline constexpr double kCopcCenterX  = 637905.545;
inline constexpr double kCopcCenterY  = 851209.905;
inline constexpr double kCopcCenterZ  = 2733.895;
inline constexpr double kCopcHalfSize = 2327.755;
inline constexpr double kCopcSpacing  = 36.37117187500007;

// Rot-hierarkin ligger sist i filen. 8896 byte / 32 byte per nod = 278 noder.
inline constexpr uint64_t kCopcRootHierOffset = 81114146;
inline constexpr uint64_t kCopcRootHierSize   = 8896;
inline constexpr uint32_t kCopcRootHierNodes  = 278;

inline bool exists(const char* path) {
    std::ifstream f = ifstream_utf8(path);
    return f.good();
}

} // namespace testdata

// Hoppar över resten av testfunktionen när testdata saknas — i stället för att
// köra vidare mot en misslyckad open och segfaulta.
#define SKIP_IF_MISSING(path)                                                  \
    do {                                                                       \
        if (!testdata::exists(path)) {                                         \
            std::printf("  SKIP — %s saknas\n", (path));                       \
            return 0;                                                          \
        }                                                                      \
    } while (0)
