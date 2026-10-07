// Writes a complete, valid SEG-Y file for the app's own generated datasets
// (Calculator, Bandpass, Save SEG-Y -- see native/README.md). The read side
// (segy_format.h/segy_decode.h, segy_mmap_*) has no counterpart for this --
// this app only ever read SEG-Y before these features existed.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "segy_format.h"

namespace segy {

struct WriteTrace {
    // Exactly kTraceHeaderSize (240) on-disk bytes, big-endian, as a real
    // SEG-Y reader expects -- typically copied verbatim from a source
    // file's mmap'd trace header rather than synthesized field-by-field,
    // since this app only understands/needs a handful of those 240 bytes
    // (see TraceHeader) but must still write back a complete, valid header.
    std::vector<uint8_t> headerBytes;
    // Exactly binHeader.samplesPerTrace values, written as big-endian IEEE
    // float32 regardless of the source's original sample format.
    std::vector<float> samples;
};

// Writes `path`: `textHeaderAscii` (EBCDIC-encoded internally via
// encodeEbcdicText), then `binHeader` (formatCode is forced to
// kIeeeFloat32 -- every dataset this app computes is written as IEEE
// float32, never re-quantized into the source's original encoding, so this
// never needs an IBM-float or fixed-point encoder), then each trace's own
// header bytes followed by its samples. Returns false (with `*error` set to
// a message fit to show the user) on any I/O failure. Does not validate
// `traces[i].headerBytes.size()`/`samples.size()` against
// kTraceHeaderSize/binHeader.samplesPerTrace -- callers own that.
bool writeSegyFile(const std::filesystem::path& path, const std::string& textHeaderAscii, BinaryHeader binHeader,
                    const std::vector<WriteTrace>& traces, std::string* error);

} // namespace segy
