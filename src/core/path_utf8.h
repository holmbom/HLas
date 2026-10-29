#pragma once

// Sökvägar är UTF-8 i hela det publika API:t, på alla plattformar.
//
// På POSIX är det gratis — byten går rakt igenom. På Windows är det inte det:
// CreateFileA, fopen och MoveFileExA tolkar byten i den aktiva ANSI-kodsidan,
// inte som UTF-8. En sökväg som `C:\Projekt\Sträcka Öst\` ger då FileNotFound,
// alltså exakt samma symptom som en flyttad fil — inget säger att det handlar
// om teckenkodning.
//
// Lösningen är att gå via W-varianterna. Konverteringen sker här, på ett ställe,
// så att varje filöppning i biblioteket använder samma stig.

#include <cstdio>
#include <string>

#ifdef _WIN32

// windows.h definierar makrona `min` och `max`, och de sprider sig till VARJE
// fil som når hit via en include. Ett `std::min(a, b)` blir då `std::(a, b)` och
// felet pekar på std::min-raden, inte på den här — alltså långt ifrån orsaken.
// Just den här headern är enda stället som drar in windows.h i biblioteket, så
// spärren hör hemma här och inte hos varje anropare.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

namespace HLas::detail {

// Tom sträng betyder misslyckad konvertering — ogiltig UTF-8, eller en sökväg
// som inte får plats. Anroparen ska då rapportera FileNotFound.
inline std::wstring utf8_to_wide(const char* path) {
    if (!path || !*path) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         path, -1, nullptr, 0);
    if (need <= 0) return std::wstring();
    // Allokera för hela `need`, inklusive avslutande NUL, och korta av efteråt.
    // Att sätta storleken till need-1 och ändå låta MultiByteToWideChar skriva
    // `need` tecken innebär en skrivning på data()[size()], vilket är strängens
    // terminator och inte vår att röra.
    std::wstring w(static_cast<size_t>(need), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            path, -1, w.data(), need) <= 0)
        return std::wstring();
    w.resize(static_cast<size_t>(need - 1));   // bort med terminatorn
    return w;
}

// Motsatta riktningen, för det som KOMMER ur Windows som UTF-16 och ska in i ett
// API som lovar UTF-8: _wgetenv, GetTempPathW, kommandoraden. Att läsa samma
// värde med getenv() ger byten i den aktiva ANSI-kodsidan, alltså inte UTF-8, och
// ett TEMP under `C:\Users\Åsa\` avvisas då av utf8_to_wide — vilket ser ut som
// en ogiltig sökväg fast det var läsningen av den som var fel.
inline std::string wide_to_utf8(const wchar_t* w) {
    if (!w || !*w) return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, w, -1,
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    // Samma resonemang som i utf8_to_wide: allokera för terminatorn och korta av
    // efteråt, i stället för att låta API:t skriva på data()[size()].
    std::string s(static_cast<size_t>(need), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), need,
                            nullptr, nullptr) <= 0)
        return std::string();
    s.resize(static_cast<size_t>(need - 1));
    return s;
}

// Sökvägar bärs som std::string på de flesta anropsställen. Utan den här
// överlagringen blir varje anrop ett `.c_str()`, och det är precis den sortens
// friktion som lockar någon att i stället nå efter fopen() med byten som de
// ligger — alltså tillbaka till ANSI-tolkningen den här filen finns för.
inline std::wstring utf8_to_wide(const std::string& path) {
    return utf8_to_wide(path.c_str());
}

} // namespace HLas::detail
#endif // _WIN32

// ---------------------------------------------------------------------------
// Radering och namnbyte, på båda plattformarna.
//
// std::remove() är samma fälla som fopen(): på Windows går den via A-varianten
// och tolkar byten i ANSI-kodsidan. En misslyckad radering kraschar inget — den
// lämnar en .tmp-fil kvar bredvid användarens data, för alltid, och bara för
// sökvägar med tecken utanför kodsidan. Alltså ett fel som inte syns i något
// test som kör i C:\Temp.

namespace HLas::detail {

inline bool remove_utf8(const std::string& path) {
#ifdef _WIN32
    const std::wstring w = utf8_to_wide(path);
    if (w.empty()) return false;
    return _wremove(w.c_str()) == 0;
#else
    return std::remove(path.c_str()) == 0;
#endif
}

// Byte av namn, på båda plattformarna. Samma ANSI-fälla som remove_utf8, och
// dessutom en semantisk skillnad: std::rename får inte skriva över en befintlig
// fil på Windows, medan POSIX rename() gör det. MOVEFILE_REPLACE_EXISTING är
// alltså inte en extra bekvämlighet — utan den beter sig de två plattformarna
// olika för precis det atomiska .tmp-till-slutgiltig-steg som skrivningen bygger
// på.
inline bool rename_utf8(const std::string& from, const std::string& to) {
#ifdef _WIN32
    const std::wstring wf = utf8_to_wide(from), wt = utf8_to_wide(to);
    if (wf.empty() || wt.empty()) return false;
    return MoveFileExW(wf.c_str(), wt.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

} // namespace HLas::detail
