#include "segy_format.h"

#include <array>
#include <cstring>

namespace segy {

int sampleFormatSizeBytes(int16_t formatCode) {
    switch (formatCode) {
        case kIbmFloat32:  return 4;
        case kInt32:       return 4;
        case kInt16:       return 2;
        case kIeeeFloat32: return 4;
        case kIeeeFloat64: return 8;
        case kInt24:       return 3;
        case kInt8:        return 1;
        default:           return 0;
    }
}

bool isSampleFormatSupported(int16_t formatCode) {
    return sampleFormatSizeBytes(formatCode) != 0;
}

BinaryHeader parseBinaryHeader(const uint8_t* b) {
    BinaryHeader h;
    // Byte positions below are 1-based SEG-Y spec positions relative to the
    // start of the file; `b` already points at offset 3200, so subtract 3200
    // (then -1 for 1-based -> 0-based) to get the offset within `b`.
    h.jobId             = readI32BE(b + (3201 - 1 - 3200));
    h.lineNumber        = readI32BE(b + (3205 - 1 - 3200));
    h.tracesPerEnsemble = readI16BE(b + (3213 - 1 - 3200));
    h.sampleIntervalUs  = readI16BE(b + (3217 - 1 - 3200));
    h.samplesPerTrace   = readI16BE(b + (3221 - 1 - 3200));
    h.formatCode        = readI16BE(b + (3225 - 1 - 3200));
    h.measurementSystem = readI16BE(b + (3255 - 1 - 3200));
    h.segyRevision      = readI16BE(b + (3501 - 1 - 3200));
    return h;
}

void writeBinaryHeader(uint8_t* b, const BinaryHeader& h) {
    std::memset(b, 0, kBinaryHeaderSize);
    writeI32BE(b + (3201 - 1 - 3200), h.jobId);
    writeI32BE(b + (3205 - 1 - 3200), h.lineNumber);
    writeI16BE(b + (3213 - 1 - 3200), h.tracesPerEnsemble);
    writeI16BE(b + (3217 - 1 - 3200), h.sampleIntervalUs);
    writeI16BE(b + (3221 - 1 - 3200), h.samplesPerTrace);
    writeI16BE(b + (3225 - 1 - 3200), h.formatCode);
    writeI16BE(b + (3255 - 1 - 3200), h.measurementSystem);
    writeI16BE(b + (3501 - 1 - 3200), h.segyRevision);
}

TraceHeader parseTraceHeader(const uint8_t* t) {
    TraceHeader h;
    h.traceSequenceLine = readI32BE(t + 0);   // bytes 1-4
    h.traceSequenceFile = readI32BE(t + 4);   // bytes 5-8
    h.fieldRecord        = readI32BE(t + 8);  // bytes 9-12
    h.traceNumber        = readI32BE(t + 12); // bytes 13-16
    h.cdp                = readI32BE(t + 20); // bytes 21-24
    h.x                  = readI32BE(t + 72); // bytes 73-76
    h.y                  = readI32BE(t + 76); // bytes 77-80
    return h;
}

namespace {
// EBCDIC (code page 037-ish, matches the table historically used by SEG-Y
// text headers) -> ASCII lookup table.
constexpr uint8_t kEbcdicToAscii[256] = {
    0,   1,   2,   3,   156, 9,   134, 127, 151, 141, 142, 11,  12,  13,  14,  15,
    16,  17,  18,  19,  157, 133, 8,   135, 24,  25,  146, 143, 28,  29,  30,  31,
    128, 129, 130, 131, 132, 10,  23,  27,  136, 137, 138, 139, 140, 5,   6,   7,
    144, 145, 22,  147, 148, 149, 150, 4,   152, 153, 154, 155, 20,  21,  158, 26,
    32,  160, 161, 162, 163, 164, 165, 166, 167, 168, 91,  46,  60,  40,  43,  33,
    38,  169, 170, 171, 172, 173, 174, 175, 176, 177, 93,  36,  42,  41,  59,  94,
    45,  47,  178, 179, 180, 181, 182, 183, 184, 185, 124, 44,  37,  95,  62,  63,
    186, 187, 188, 189, 190, 191, 192, 193, 194, 96,  58,  35,  64,  39,  61,  34,
    195, 97,  98,  99,  100, 101, 102, 103, 104, 105, 196, 197, 198, 199, 200, 201,
    202, 106, 107, 108, 109, 110, 111, 112, 113, 114, 203, 204, 205, 206, 207, 208,
    209, 126, 115, 116, 117, 118, 119, 120, 121, 122, 210, 211, 212, 213, 214, 215,
    216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231,
    123, 65,  66,  67,  68,  69,  70,  71,  72,  73,  232, 233, 234, 235, 236, 237,
    125, 74,  75,  76,  77,  78,  79,  80,  81,  82,  238, 239, 240, 241, 242, 243,
    92,  159, 83,  84,  85,  86,  87,  88,  89,  90,  244, 245, 246, 247, 248, 249,
    48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  250, 251, 252, 253, 254, 255,
};
} // namespace

std::string decodeEbcdicText(const uint8_t* src3200) {
    // Despite this function's name/the historical convention, not every
    // SEG-Y file's text header is actually EBCDIC -- files written by
    // modern, non-mainframe tools sometimes write plain ASCII instead, and
    // nothing in the file format itself flags which one was used. Running
    // ASCII bytes through the EBCDIC table anyway produces pure gibberish,
    // so detect which one this is first, rather than assuming.
    //
    // Counting letters/digits specifically (not "printable ASCII" broadly)
    // matters here: EBCDIC space is 0x40, which happens to equal ASCII '@'
    // -- a real EBCDIC header that's mostly blank-padded (common; often
    // only the first few of its 40 lines hold actual text) would otherwise
    // get misread as "mostly printable ASCII" purely from its padding.
    // EBCDIC's own letter/digit bytes (0xC1-0xE9 for A-Z, 0xF0-0xF9 for
    // 0-9) are entirely disjoint from ASCII's (0x30-0x39, 0x41-0x5A,
    // 0x61-0x7A), so real text in either encoding lands almost exclusively
    // in one count or the other, blank padding contributing to neither.
    size_t asciiAlnum = 0, ebcdicAlnum = 0;
    for (size_t i = 0; i < kTextHeaderSize; ++i) {
        uint8_t b = src3200[i];
        if ((b >= '0' && b <= '9') || (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z')) ++asciiAlnum;
        if ((b >= 0xC1 && b <= 0xC9) || (b >= 0xD1 && b <= 0xD9) || (b >= 0xE2 && b <= 0xE9) ||
            (b >= 0xF0 && b <= 0xF9)) {
            ++ebcdicAlnum;
        }
    }
    if (asciiAlnum > ebcdicAlnum) {
        return std::string(reinterpret_cast<const char*>(src3200), kTextHeaderSize);
    }

    std::string out;
    out.resize(kTextHeaderSize);
    for (size_t i = 0; i < kTextHeaderSize; ++i) {
        out[i] = char(kEbcdicToAscii[src3200[i]]);
    }
    return out;
}

std::string encodeEbcdicText(const std::string& asciiText) {
    // Built once from kEbcdicToAscii itself (the exact inverse of the table
    // decodeEbcdicText already uses) rather than a second hand-transcribed
    // table that could silently drift out of sync with it.
    static const std::array<uint8_t, 256> kAsciiToEbcdic = [] {
        std::array<uint8_t, 256> table{};
        table.fill(0x40); // EBCDIC space -- fallback for any ASCII byte with no mapping below
        for (int ebcdic = 0; ebcdic < 256; ++ebcdic) table[kEbcdicToAscii[ebcdic]] = uint8_t(ebcdic);
        return table;
    }();

    std::string out(kTextHeaderSize, '\0');
    for (size_t i = 0; i < kTextHeaderSize; ++i) {
        uint8_t ascii = i < asciiText.size() ? uint8_t(asciiText[i]) : uint8_t(' ');
        out[i] = char(kAsciiToEbcdic[ascii]);
    }
    return out;
}

} // namespace segy
