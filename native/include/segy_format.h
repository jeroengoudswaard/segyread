// SEG-Y on-disk layout: constants, big-endian readers, and header parsing.
// Reference: SEG-Y rev 1/2 (3200-byte EBCDIC text header, 400-byte binary
// header, then traces of 240-byte header + samples).
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace segy {

constexpr size_t kTextHeaderSize = 3200;
constexpr size_t kBinaryHeaderSize = 400;
constexpr size_t kTraceHeaderSize = 240;
constexpr size_t kHeaderTotalSize = kTextHeaderSize + kBinaryHeaderSize; // 3600

// SEG-Y sample format codes (binary header bytes 3225-3226).
enum SampleFormat : int16_t {
    kIbmFloat32  = 1,
    kInt32       = 2,
    kInt16       = 3,
    kFixedGain   = 4, // obsolete "gain code" format, not supported
    kIeeeFloat32 = 5,
    kIeeeFloat64 = 6,
    kInt24       = 7,
    kInt8        = 8,
};

// Returns the on-disk size in bytes of one sample for a given format code,
// or 0 if the format is not recognized/supported.
int sampleFormatSizeBytes(int16_t formatCode);
bool isSampleFormatSupported(int16_t formatCode);

// SEG-Y is big-endian on disk by spec; these read directly from a raw byte
// pointer so callers can point straight into a memory-mapped file with no
// intermediate copy.
inline uint16_t readU16BE(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}
inline int16_t readI16BE(const uint8_t* p) {
    return int16_t(readU16BE(p));
}
inline uint32_t readU32BE(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline int32_t readI32BE(const uint8_t* p) {
    return int32_t(readU32BE(p));
}

// Write-side counterparts, for segy_writer.cpp's synthesized-dataset output
// (Calculator, Bandpass, Save SEG-Y -- see native/README.md).
inline void writeU16BE(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}
inline void writeI16BE(uint8_t* p, int16_t v) { writeU16BE(p, uint16_t(v)); }
inline void writeU32BE(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}
inline void writeI32BE(uint8_t* p, int32_t v) { writeU32BE(p, uint32_t(v)); }

struct BinaryHeader {
    int32_t jobId = 0;
    int32_t lineNumber = 0;
    int16_t tracesPerEnsemble = 0;
    int16_t samplesPerTrace = 0;
    int16_t sampleIntervalUs = 0;
    int16_t formatCode = 0;
    int16_t measurementSystem = 0;
    int16_t segyRevision = 0;
};

// `binHeaderBase` must point at byte offset 3200 of the file (start of the
// 400-byte binary header), i.e. file base + kTextHeaderSize.
BinaryHeader parseBinaryHeader(const uint8_t* binHeaderBase);

// Inverse of parseBinaryHeader: serializes `h`'s known fields into a
// kBinaryHeaderSize buffer at the standard SEG-Y byte offsets, zero-filling
// everything else (unknown/reserved fields) -- so a file this app writes and
// then reopens itself round-trips through exactly the same field set.
void writeBinaryHeader(uint8_t* binHeaderBase, const BinaryHeader& h);

struct TraceHeader {
    int32_t traceSequenceLine = 0;
    int32_t traceSequenceFile = 0;
    int32_t fieldRecord = 0;
    int32_t traceNumber = 0;
    int32_t cdp = 0;
    int32_t x = 0;
    int32_t y = 0;
};

// `traceHeaderBase` must point at the start of a 240-byte trace header.
TraceHeader parseTraceHeader(const uint8_t* traceHeaderBase);

// Decodes a 3200-byte EBCDIC (IBM code page 037-ish) text header to ASCII.
std::string decodeEbcdicText(const uint8_t* src3200);

// Inverse of decodeEbcdicText: encodes ASCII `asciiText` into a
// kTextHeaderSize-byte EBCDIC (code page 037) string, padding short input
// with spaces and truncating long input -- always exactly kTextHeaderSize
// bytes out. A character with no EBCDIC counterpart in the table (non-ASCII
// input) encodes as EBCDIC space (0x40) rather than garbage.
std::string encodeEbcdicText(const std::string& asciiText);

} // namespace segy
