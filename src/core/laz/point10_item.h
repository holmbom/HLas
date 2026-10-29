#pragma once
#ifdef HLAS_HAVE_LAZ

#include <cstdint>

namespace HLas::detail::laz {

// Point10-postens 20 byte, fält för fält, plus de två kontexttabellerna.
//
// Delas av Point10Decoder och Point10Encoder av samma skäl som point14_item.h
// delas: två kopior kan glida isär, en kan inte. Tabellerna låg tidigare som
// statiska medlemmar i avkodaren, alltså oåtkomliga för kodaren — och en
// andra kopia av just de här hade varit den värsta sorten, eftersom en enda
// felaktig ruta bara syns för de returkombinationer den beskriver.
//
// Layouten är LAS punktformat 0. Format 1-5 lägger GPS-tid, RGB och
// wave packets EFTER de här 20 byten och rör dem inte.

constexpr int OFF10_X              = 0;
constexpr int OFF10_Y              = 4;
constexpr int OFF10_Z              = 8;
constexpr int OFF10_INTENSITY      = 12;
constexpr int OFF10_BIT_BYTE       = 14;  // return:3 | num_returns:3 | scan_dir:1 | edge:1
constexpr int OFF10_CLASSIFICATION = 15;  // hela byten, flaggbitarna inräknade
constexpr int OFF10_SCAN_ANGLE     = 16;  // int8, hela grader
constexpr int OFF10_USER_DATA      = 17;
constexpr int OFF10_POINT_SOURCE   = 18;  // uint16

constexpr int kPoint10Size = 20;

inline uint8_t get10_return_number(const uint8_t* p)  { return p[OFF10_BIT_BYTE] & 0x07; }
inline uint8_t get10_number_of_returns(const uint8_t* p) { return (p[OFF10_BIT_BYTE] >> 3) & 0x07; }
inline uint8_t get10_scan_direction(const uint8_t* p) { return (p[OFF10_BIT_BYTE] >> 6) & 0x01; }

// Tabellerna ur spec 13.1 (figur 23).
//
// De "riktiga" kombinationerna — retur r av n med 1 <= r <= n — behöver bara ett
// fåtal rutor. Resten finns för att filer förekommer som numrerar från noll,
// bara fyller i det ena fältet, eller kastar om dem; de mappas till egna
// kontexter i stället för att klumpas ihop med de giltiga.
inline constexpr uint8_t kReturnMap[8][8] = {
    { 15, 14, 13, 12, 11, 10,  9,  8 },
    { 14,  0,  1,  3,  6, 10, 10,  9 },
    { 13,  1,  2,  4,  7, 11, 11, 10 },
    { 12,  3,  4,  5,  8, 12, 12, 11 },
    { 11,  6,  7,  8,  9, 13, 13, 12 },
    { 10, 10, 11, 12, 13, 14, 14, 13 },
    {  9, 10, 11, 12, 13, 14, 15, 14 },
    {  8,  9, 10, 11, 12, 13, 14, 15 },
};

inline constexpr uint8_t kReturnLevel[8][8] = {
    {  0,  1,  2,  3,  4,  5,  6,  7 },
    {  1,  0,  1,  2,  3,  4,  5,  6 },
    {  2,  1,  0,  1,  2,  3,  4,  5 },
    {  3,  2,  1,  0,  1,  2,  3,  4 },
    {  4,  3,  2,  1,  0,  1,  2,  3 },
    {  5,  4,  3,  2,  1,  0,  1,  2 },
    {  6,  5,  4,  3,  2,  1,  0,  1 },
    {  7,  6,  5,  4,  3,  2,  1,  0 },
};

} // namespace HLas::detail::laz

#endif // HLAS_HAVE_LAZ
