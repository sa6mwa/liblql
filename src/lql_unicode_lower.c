#include "lql_unicode_lower.h"

#include <stddef.h>

#define LQL_UNICODE_UPPER_LOWER 1114112l

typedef struct lql_unicode_lower_range {
  unsigned long first;
  unsigned long last;
  long lower_delta;
} lql_unicode_lower_range;

/* Generated from Go 1.26.4 unicode.CaseRanges (Unicode 15.0.0). */
static const lql_unicode_lower_range lql_unicode_lower_ranges[] = {
    {0x0041ul, 0x005Aul, 32l},      {0x0061ul, 0x007Aul, 0l},
    {0x00B5ul, 0x00B5ul, 0l},       {0x00C0ul, 0x00D6ul, 32l},
    {0x00D8ul, 0x00DEul, 32l},      {0x00E0ul, 0x00F6ul, 0l},
    {0x00F8ul, 0x00FEul, 0l},       {0x00FFul, 0x00FFul, 0l},
    {0x0100ul, 0x012Ful, 1114112l}, {0x0130ul, 0x0130ul, -199l},
    {0x0131ul, 0x0131ul, 0l},       {0x0132ul, 0x0137ul, 1114112l},
    {0x0139ul, 0x0148ul, 1114112l}, {0x014Aul, 0x0177ul, 1114112l},
    {0x0178ul, 0x0178ul, -121l},    {0x0179ul, 0x017Eul, 1114112l},
    {0x017Ful, 0x017Ful, 0l},       {0x0180ul, 0x0180ul, 0l},
    {0x0181ul, 0x0181ul, 210l},     {0x0182ul, 0x0185ul, 1114112l},
    {0x0186ul, 0x0186ul, 206l},     {0x0187ul, 0x0188ul, 1114112l},
    {0x0189ul, 0x018Aul, 205l},     {0x018Bul, 0x018Cul, 1114112l},
    {0x018Eul, 0x018Eul, 79l},      {0x018Ful, 0x018Ful, 202l},
    {0x0190ul, 0x0190ul, 203l},     {0x0191ul, 0x0192ul, 1114112l},
    {0x0193ul, 0x0193ul, 205l},     {0x0194ul, 0x0194ul, 207l},
    {0x0195ul, 0x0195ul, 0l},       {0x0196ul, 0x0196ul, 211l},
    {0x0197ul, 0x0197ul, 209l},     {0x0198ul, 0x0199ul, 1114112l},
    {0x019Aul, 0x019Aul, 0l},       {0x019Cul, 0x019Cul, 211l},
    {0x019Dul, 0x019Dul, 213l},     {0x019Eul, 0x019Eul, 0l},
    {0x019Ful, 0x019Ful, 214l},     {0x01A0ul, 0x01A5ul, 1114112l},
    {0x01A6ul, 0x01A6ul, 218l},     {0x01A7ul, 0x01A8ul, 1114112l},
    {0x01A9ul, 0x01A9ul, 218l},     {0x01ACul, 0x01ADul, 1114112l},
    {0x01AEul, 0x01AEul, 218l},     {0x01AFul, 0x01B0ul, 1114112l},
    {0x01B1ul, 0x01B2ul, 217l},     {0x01B3ul, 0x01B6ul, 1114112l},
    {0x01B7ul, 0x01B7ul, 219l},     {0x01B8ul, 0x01B9ul, 1114112l},
    {0x01BCul, 0x01BDul, 1114112l}, {0x01BFul, 0x01BFul, 0l},
    {0x01C4ul, 0x01C4ul, 2l},       {0x01C5ul, 0x01C5ul, 1l},
    {0x01C6ul, 0x01C6ul, 0l},       {0x01C7ul, 0x01C7ul, 2l},
    {0x01C8ul, 0x01C8ul, 1l},       {0x01C9ul, 0x01C9ul, 0l},
    {0x01CAul, 0x01CAul, 2l},       {0x01CBul, 0x01CBul, 1l},
    {0x01CCul, 0x01CCul, 0l},       {0x01CDul, 0x01DCul, 1114112l},
    {0x01DDul, 0x01DDul, 0l},       {0x01DEul, 0x01EFul, 1114112l},
    {0x01F1ul, 0x01F1ul, 2l},       {0x01F2ul, 0x01F2ul, 1l},
    {0x01F3ul, 0x01F3ul, 0l},       {0x01F4ul, 0x01F5ul, 1114112l},
    {0x01F6ul, 0x01F6ul, -97l},     {0x01F7ul, 0x01F7ul, -56l},
    {0x01F8ul, 0x021Ful, 1114112l}, {0x0220ul, 0x0220ul, -130l},
    {0x0222ul, 0x0233ul, 1114112l}, {0x023Aul, 0x023Aul, 10795l},
    {0x023Bul, 0x023Cul, 1114112l}, {0x023Dul, 0x023Dul, -163l},
    {0x023Eul, 0x023Eul, 10792l},   {0x023Ful, 0x0240ul, 0l},
    {0x0241ul, 0x0242ul, 1114112l}, {0x0243ul, 0x0243ul, -195l},
    {0x0244ul, 0x0244ul, 69l},      {0x0245ul, 0x0245ul, 71l},
    {0x0246ul, 0x024Ful, 1114112l}, {0x0250ul, 0x0250ul, 0l},
    {0x0251ul, 0x0251ul, 0l},       {0x0252ul, 0x0252ul, 0l},
    {0x0253ul, 0x0253ul, 0l},       {0x0254ul, 0x0254ul, 0l},
    {0x0256ul, 0x0257ul, 0l},       {0x0259ul, 0x0259ul, 0l},
    {0x025Bul, 0x025Bul, 0l},       {0x025Cul, 0x025Cul, 0l},
    {0x0260ul, 0x0260ul, 0l},       {0x0261ul, 0x0261ul, 0l},
    {0x0263ul, 0x0263ul, 0l},       {0x0265ul, 0x0265ul, 0l},
    {0x0266ul, 0x0266ul, 0l},       {0x0268ul, 0x0268ul, 0l},
    {0x0269ul, 0x0269ul, 0l},       {0x026Aul, 0x026Aul, 0l},
    {0x026Bul, 0x026Bul, 0l},       {0x026Cul, 0x026Cul, 0l},
    {0x026Ful, 0x026Ful, 0l},       {0x0271ul, 0x0271ul, 0l},
    {0x0272ul, 0x0272ul, 0l},       {0x0275ul, 0x0275ul, 0l},
    {0x027Dul, 0x027Dul, 0l},       {0x0280ul, 0x0280ul, 0l},
    {0x0282ul, 0x0282ul, 0l},       {0x0283ul, 0x0283ul, 0l},
    {0x0287ul, 0x0287ul, 0l},       {0x0288ul, 0x0288ul, 0l},
    {0x0289ul, 0x0289ul, 0l},       {0x028Aul, 0x028Bul, 0l},
    {0x028Cul, 0x028Cul, 0l},       {0x0292ul, 0x0292ul, 0l},
    {0x029Dul, 0x029Dul, 0l},       {0x029Eul, 0x029Eul, 0l},
    {0x0345ul, 0x0345ul, 0l},       {0x0370ul, 0x0373ul, 1114112l},
    {0x0376ul, 0x0377ul, 1114112l}, {0x037Bul, 0x037Dul, 0l},
    {0x037Ful, 0x037Ful, 116l},     {0x0386ul, 0x0386ul, 38l},
    {0x0388ul, 0x038Aul, 37l},      {0x038Cul, 0x038Cul, 64l},
    {0x038Eul, 0x038Ful, 63l},      {0x0391ul, 0x03A1ul, 32l},
    {0x03A3ul, 0x03ABul, 32l},      {0x03ACul, 0x03ACul, 0l},
    {0x03ADul, 0x03AFul, 0l},       {0x03B1ul, 0x03C1ul, 0l},
    {0x03C2ul, 0x03C2ul, 0l},       {0x03C3ul, 0x03CBul, 0l},
    {0x03CCul, 0x03CCul, 0l},       {0x03CDul, 0x03CEul, 0l},
    {0x03CFul, 0x03CFul, 8l},       {0x03D0ul, 0x03D0ul, 0l},
    {0x03D1ul, 0x03D1ul, 0l},       {0x03D5ul, 0x03D5ul, 0l},
    {0x03D6ul, 0x03D6ul, 0l},       {0x03D7ul, 0x03D7ul, 0l},
    {0x03D8ul, 0x03EFul, 1114112l}, {0x03F0ul, 0x03F0ul, 0l},
    {0x03F1ul, 0x03F1ul, 0l},       {0x03F2ul, 0x03F2ul, 0l},
    {0x03F3ul, 0x03F3ul, 0l},       {0x03F4ul, 0x03F4ul, -60l},
    {0x03F5ul, 0x03F5ul, 0l},       {0x03F7ul, 0x03F8ul, 1114112l},
    {0x03F9ul, 0x03F9ul, -7l},      {0x03FAul, 0x03FBul, 1114112l},
    {0x03FDul, 0x03FFul, -130l},    {0x0400ul, 0x040Ful, 80l},
    {0x0410ul, 0x042Ful, 32l},      {0x0430ul, 0x044Ful, 0l},
    {0x0450ul, 0x045Ful, 0l},       {0x0460ul, 0x0481ul, 1114112l},
    {0x048Aul, 0x04BFul, 1114112l}, {0x04C0ul, 0x04C0ul, 15l},
    {0x04C1ul, 0x04CEul, 1114112l}, {0x04CFul, 0x04CFul, 0l},
    {0x04D0ul, 0x052Ful, 1114112l}, {0x0531ul, 0x0556ul, 48l},
    {0x0561ul, 0x0586ul, 0l},       {0x10A0ul, 0x10C5ul, 7264l},
    {0x10C7ul, 0x10C7ul, 7264l},    {0x10CDul, 0x10CDul, 7264l},
    {0x10D0ul, 0x10FAul, 0l},       {0x10FDul, 0x10FFul, 0l},
    {0x13A0ul, 0x13EFul, 38864l},   {0x13F0ul, 0x13F5ul, 8l},
    {0x13F8ul, 0x13FDul, 0l},       {0x1C80ul, 0x1C80ul, 0l},
    {0x1C81ul, 0x1C81ul, 0l},       {0x1C82ul, 0x1C82ul, 0l},
    {0x1C83ul, 0x1C84ul, 0l},       {0x1C85ul, 0x1C85ul, 0l},
    {0x1C86ul, 0x1C86ul, 0l},       {0x1C87ul, 0x1C87ul, 0l},
    {0x1C88ul, 0x1C88ul, 0l},       {0x1C90ul, 0x1CBAul, -3008l},
    {0x1CBDul, 0x1CBFul, -3008l},   {0x1D79ul, 0x1D79ul, 0l},
    {0x1D7Dul, 0x1D7Dul, 0l},       {0x1D8Eul, 0x1D8Eul, 0l},
    {0x1E00ul, 0x1E95ul, 1114112l}, {0x1E9Bul, 0x1E9Bul, 0l},
    {0x1E9Eul, 0x1E9Eul, -7615l},   {0x1EA0ul, 0x1EFFul, 1114112l},
    {0x1F00ul, 0x1F07ul, 0l},       {0x1F08ul, 0x1F0Ful, -8l},
    {0x1F10ul, 0x1F15ul, 0l},       {0x1F18ul, 0x1F1Dul, -8l},
    {0x1F20ul, 0x1F27ul, 0l},       {0x1F28ul, 0x1F2Ful, -8l},
    {0x1F30ul, 0x1F37ul, 0l},       {0x1F38ul, 0x1F3Ful, -8l},
    {0x1F40ul, 0x1F45ul, 0l},       {0x1F48ul, 0x1F4Dul, -8l},
    {0x1F51ul, 0x1F51ul, 0l},       {0x1F53ul, 0x1F53ul, 0l},
    {0x1F55ul, 0x1F55ul, 0l},       {0x1F57ul, 0x1F57ul, 0l},
    {0x1F59ul, 0x1F59ul, -8l},      {0x1F5Bul, 0x1F5Bul, -8l},
    {0x1F5Dul, 0x1F5Dul, -8l},      {0x1F5Ful, 0x1F5Ful, -8l},
    {0x1F60ul, 0x1F67ul, 0l},       {0x1F68ul, 0x1F6Ful, -8l},
    {0x1F70ul, 0x1F71ul, 0l},       {0x1F72ul, 0x1F75ul, 0l},
    {0x1F76ul, 0x1F77ul, 0l},       {0x1F78ul, 0x1F79ul, 0l},
    {0x1F7Aul, 0x1F7Bul, 0l},       {0x1F7Cul, 0x1F7Dul, 0l},
    {0x1F80ul, 0x1F87ul, 0l},       {0x1F88ul, 0x1F8Ful, -8l},
    {0x1F90ul, 0x1F97ul, 0l},       {0x1F98ul, 0x1F9Ful, -8l},
    {0x1FA0ul, 0x1FA7ul, 0l},       {0x1FA8ul, 0x1FAFul, -8l},
    {0x1FB0ul, 0x1FB1ul, 0l},       {0x1FB3ul, 0x1FB3ul, 0l},
    {0x1FB8ul, 0x1FB9ul, -8l},      {0x1FBAul, 0x1FBBul, -74l},
    {0x1FBCul, 0x1FBCul, -9l},      {0x1FBEul, 0x1FBEul, 0l},
    {0x1FC3ul, 0x1FC3ul, 0l},       {0x1FC8ul, 0x1FCBul, -86l},
    {0x1FCCul, 0x1FCCul, -9l},      {0x1FD0ul, 0x1FD1ul, 0l},
    {0x1FD8ul, 0x1FD9ul, -8l},      {0x1FDAul, 0x1FDBul, -100l},
    {0x1FE0ul, 0x1FE1ul, 0l},       {0x1FE5ul, 0x1FE5ul, 0l},
    {0x1FE8ul, 0x1FE9ul, -8l},      {0x1FEAul, 0x1FEBul, -112l},
    {0x1FECul, 0x1FECul, -7l},      {0x1FF3ul, 0x1FF3ul, 0l},
    {0x1FF8ul, 0x1FF9ul, -128l},    {0x1FFAul, 0x1FFBul, -126l},
    {0x1FFCul, 0x1FFCul, -9l},      {0x2126ul, 0x2126ul, -7517l},
    {0x212Aul, 0x212Aul, -8383l},   {0x212Bul, 0x212Bul, -8262l},
    {0x2132ul, 0x2132ul, 28l},      {0x214Eul, 0x214Eul, 0l},
    {0x2160ul, 0x216Ful, 16l},      {0x2170ul, 0x217Ful, 0l},
    {0x2183ul, 0x2184ul, 1114112l}, {0x24B6ul, 0x24CFul, 26l},
    {0x24D0ul, 0x24E9ul, 0l},       {0x2C00ul, 0x2C2Ful, 48l},
    {0x2C30ul, 0x2C5Ful, 0l},       {0x2C60ul, 0x2C61ul, 1114112l},
    {0x2C62ul, 0x2C62ul, -10743l},  {0x2C63ul, 0x2C63ul, -3814l},
    {0x2C64ul, 0x2C64ul, -10727l},  {0x2C65ul, 0x2C65ul, 0l},
    {0x2C66ul, 0x2C66ul, 0l},       {0x2C67ul, 0x2C6Cul, 1114112l},
    {0x2C6Dul, 0x2C6Dul, -10780l},  {0x2C6Eul, 0x2C6Eul, -10749l},
    {0x2C6Ful, 0x2C6Ful, -10783l},  {0x2C70ul, 0x2C70ul, -10782l},
    {0x2C72ul, 0x2C73ul, 1114112l}, {0x2C75ul, 0x2C76ul, 1114112l},
    {0x2C7Eul, 0x2C7Ful, -10815l},  {0x2C80ul, 0x2CE3ul, 1114112l},
    {0x2CEBul, 0x2CEEul, 1114112l}, {0x2CF2ul, 0x2CF3ul, 1114112l},
    {0x2D00ul, 0x2D25ul, 0l},       {0x2D27ul, 0x2D27ul, 0l},
    {0x2D2Dul, 0x2D2Dul, 0l},       {0xA640ul, 0xA66Dul, 1114112l},
    {0xA680ul, 0xA69Bul, 1114112l}, {0xA722ul, 0xA72Ful, 1114112l},
    {0xA732ul, 0xA76Ful, 1114112l}, {0xA779ul, 0xA77Cul, 1114112l},
    {0xA77Dul, 0xA77Dul, -35332l},  {0xA77Eul, 0xA787ul, 1114112l},
    {0xA78Bul, 0xA78Cul, 1114112l}, {0xA78Dul, 0xA78Dul, -42280l},
    {0xA790ul, 0xA793ul, 1114112l}, {0xA794ul, 0xA794ul, 0l},
    {0xA796ul, 0xA7A9ul, 1114112l}, {0xA7AAul, 0xA7AAul, -42308l},
    {0xA7ABul, 0xA7ABul, -42319l},  {0xA7ACul, 0xA7ACul, -42315l},
    {0xA7ADul, 0xA7ADul, -42305l},  {0xA7AEul, 0xA7AEul, -42308l},
    {0xA7B0ul, 0xA7B0ul, -42258l},  {0xA7B1ul, 0xA7B1ul, -42282l},
    {0xA7B2ul, 0xA7B2ul, -42261l},  {0xA7B3ul, 0xA7B3ul, 928l},
    {0xA7B4ul, 0xA7C3ul, 1114112l}, {0xA7C4ul, 0xA7C4ul, -48l},
    {0xA7C5ul, 0xA7C5ul, -42307l},  {0xA7C6ul, 0xA7C6ul, -35384l},
    {0xA7C7ul, 0xA7CAul, 1114112l}, {0xA7D0ul, 0xA7D1ul, 1114112l},
    {0xA7D6ul, 0xA7D9ul, 1114112l}, {0xA7F5ul, 0xA7F6ul, 1114112l},
    {0xAB53ul, 0xAB53ul, 0l},       {0xAB70ul, 0xABBFul, 0l},
    {0xFF21ul, 0xFF3Aul, 32l},      {0xFF41ul, 0xFF5Aul, 0l},
    {0x10400ul, 0x10427ul, 40l},    {0x10428ul, 0x1044Ful, 0l},
    {0x104B0ul, 0x104D3ul, 40l},    {0x104D8ul, 0x104FBul, 0l},
    {0x10570ul, 0x1057Aul, 39l},    {0x1057Cul, 0x1058Aul, 39l},
    {0x1058Cul, 0x10592ul, 39l},    {0x10594ul, 0x10595ul, 39l},
    {0x10597ul, 0x105A1ul, 0l},     {0x105A3ul, 0x105B1ul, 0l},
    {0x105B3ul, 0x105B9ul, 0l},     {0x105BBul, 0x105BCul, 0l},
    {0x10C80ul, 0x10CB2ul, 64l},    {0x10CC0ul, 0x10CF2ul, 0l},
    {0x118A0ul, 0x118BFul, 32l},    {0x118C0ul, 0x118DFul, 0l},
    {0x16E40ul, 0x16E5Ful, 32l},    {0x16E60ul, 0x16E7Ful, 0l},
    {0x1E900ul, 0x1E921ul, 34l},    {0x1E922ul, 0x1E943ul, 0l},
};

unsigned long lql_unicode_simple_lower(unsigned long value) {
  size_t first;
  size_t last;
  if (value >= (unsigned long)'A' && value <= (unsigned long)'Z') {
    return value + ((unsigned long)'a' - (unsigned long)'A');
  }
  first = 0u;
  last = sizeof(lql_unicode_lower_ranges) / sizeof(lql_unicode_lower_ranges[0]);
  while (first < last) {
    size_t middle;
    const lql_unicode_lower_range *range;
    middle = first + (last - first) / 2u;
    range = &lql_unicode_lower_ranges[middle];
    if (value < range->first) {
      last = middle;
    } else if (value > range->last) {
      first = middle + 1u;
    } else if (range->lower_delta == LQL_UNICODE_UPPER_LOWER) {
      return range->first + ((value - range->first) | 1ul);
    } else {
      return (unsigned long)((long)value + range->lower_delta);
    }
  }
  return value;
}

static int lql_unicode_utf8_decode(const unsigned char *input, size_t input_len,
                                   size_t *offset, unsigned long *out) {
  unsigned long value;
  size_t width;
  unsigned char first;
  if (input == NULL || offset == NULL || out == NULL || *offset >= input_len) {
    return 0;
  }
  first = input[*offset];
  if (first < 0x80u) {
    *out = first;
    ++*offset;
    return 1;
  }
  if (first >= 0xc2u && first <= 0xdfu) {
    width = 2u;
    value = first & 0x1ful;
  } else if (first >= 0xe0u && first <= 0xefu) {
    width = 3u;
    value = first & 0x0ful;
  } else if (first >= 0xf0u && first <= 0xf4u) {
    width = 4u;
    value = first & 0x07ul;
  } else {
    return 0;
  }
  if (width > input_len - *offset) {
    return 0;
  }
  while (--width != 0u) {
    unsigned char next;
    ++*offset;
    next = input[*offset];
    if (next < 0x80u || next > 0xbfu) {
      return 0;
    }
    value = (value << 6u) | (unsigned long)(next & 0x3fu);
  }
  ++*offset;
  if ((value >= 0xd800ul && value <= 0xdffful) || value > 0x10fffful ||
      (first == 0xe0u && value < 0x800ul) ||
      (first == 0xf0u && value < 0x10000ul)) {
    return 0;
  }
  *out = value;
  return 1;
}

static int lql_unicode_utf8_append(unsigned long value, char *output,
                                   size_t output_capacity, size_t *out_len) {
  size_t size;
  if (output == NULL || out_len == NULL || value > 0x10fffful ||
      (value >= 0xd800ul && value <= 0xdffful)) {
    return 0;
  }
  size = value < 0x80ul      ? 1u
         : value < 0x800ul   ? 2u
         : value < 0x10000ul ? 3u
                             : 4u;
  if (size > output_capacity - *out_len) {
    return 0;
  }
  if (size == 1u) {
    output[(*out_len)++] = (char)value;
  } else if (size == 2u) {
    output[(*out_len)++] = (char)(0xc0u | (value >> 6u));
    output[(*out_len)++] = (char)(0x80u | (value & 0x3ful));
  } else if (size == 3u) {
    output[(*out_len)++] = (char)(0xe0u | (value >> 12u));
    output[(*out_len)++] = (char)(0x80u | ((value >> 6u) & 0x3ful));
    output[(*out_len)++] = (char)(0x80u | (value & 0x3ful));
  } else {
    output[(*out_len)++] = (char)(0xf0u | (value >> 18u));
    output[(*out_len)++] = (char)(0x80u | ((value >> 12u) & 0x3ful));
    output[(*out_len)++] = (char)(0x80u | ((value >> 6u) & 0x3ful));
    output[(*out_len)++] = (char)(0x80u | (value & 0x3ful));
  }
  return 1;
}

int lql_unicode_utf8_decode_one(const unsigned char *input, size_t input_len,
                                unsigned long *out) {
  size_t offset;
  offset = 0u;
  return lql_unicode_utf8_decode(input, input_len, &offset, out) &&
         offset == input_len;
}

size_t lql_unicode_utf8_encode(unsigned long value, unsigned char output[4]) {
  size_t len;
  if (output == NULL)
    return 0u;
  len = 0u;
  if (!lql_unicode_utf8_append(value, (char *)output, 4u, &len))
    return 0u;
  return len;
}

int lql_unicode_utf8_lower(const char *input, size_t input_len, char *output,
                           size_t output_capacity, size_t *out_len) {
  size_t offset;
  size_t output_len;
  if (input == NULL || output == NULL || out_len == NULL) {
    return 0;
  }
  offset = 0u;
  output_len = 0u;
  while (offset < input_len) {
    unsigned long value;
    if (!lql_unicode_utf8_decode((const unsigned char *)input, input_len,
                                 &offset, &value) ||
        !lql_unicode_utf8_append(lql_unicode_simple_lower(value), output,
                                 output_capacity, &output_len)) {
      return 0;
    }
  }
  *out_len = output_len;
  return 1;
}
