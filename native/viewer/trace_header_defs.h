// Trace-header field definitions for the Trace Inspector (File > Trace
// Inspector): which bytes of the 240-byte SEG-Y trace header mean what,
// per standard. Qt-free, covered by segytest (every definition tiles bytes
// 1..240 without gaps or overlaps).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace segy {

enum class HeaderStandard {
    ShellV21, // Shell "SEGY trace identing" (V2.1) -- the default
    SegyRev1, // SEG-Y revision 1.0 (2002)
    SegyRev2, // SEG-Y revision 2.0 (2017)
};

enum class HeaderFieldKind {
    Int,              // signed integer, `size` 2 or 4 bytes
    MantissaExponent, // 6 bytes: 4-byte mantissa, 2-byte power of ten
    ThreeShorts,      // 6 bytes: three 2-byte integers (e.g. source energy direction)
    Ascii,            // text (e.g. rev 2.0's "SEG00000" header name)
    Unassigned,       // not used / optional: shown as raw bytes
};

struct HeaderFieldDef {
    int firstByte = 1; // 1-based, as in the standards
    int size = 4;
    HeaderFieldKind kind = HeaderFieldKind::Int;
    const char* name = "";        // short mnemonic, may be empty
    const char* description = ""; // one line
};

const std::vector<HeaderFieldDef>& traceHeaderDefinition(HeaderStandard standard);
const char* headerStandardName(HeaderStandard standard);

// Reads a signed integer of `size` (1, 2 or 4) bytes at 1-based `firstByte`.
int64_t readHeaderInt(const uint8_t* header, int firstByte, int size, bool bigEndian);

// The field's value as display text: integers as decimal, mantissa x
// 10^exponent as a number, three shorts as "a, b, c", text as printable
// ASCII, unassigned bytes as hex (or "0" when all zero).
std::string formatHeaderField(const uint8_t* header, const HeaderFieldDef& field, bool bigEndian);

} // namespace segy
