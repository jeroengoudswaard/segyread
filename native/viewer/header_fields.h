// Trace-header fields at positions the user chooses, and everything the
// File > Open with Analysis step needs around them: field types, a mapping
// of the fields the app uses onto byte positions (saved as a template),
// header positions read from the text header, a trace selection, and the
// SEG-Y rev 2 layout (data start, extended text headers). Qt-free, covered
// by segytest. Behaviour after Ewoud's segy_reader (see COPILOT item 63):
// never guess without saying so; an unreadable layout never stops the
// headers from being shown.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace segy {

enum class FieldType { Int16, Int32, UInt16, UInt32, Ieee32, Ibm32, Int64, UInt64, Ieee64 };

int fieldTypeSize(FieldType type);
// Short names as in header documentation: 2I, 4I, 2U, 4U, 4F (IEEE), 4B
// (IBM), 8I, 8U, 8F.
const char* fieldTypeName(FieldType type);
bool parseFieldType(const std::string& text, FieldType& out);
const std::vector<FieldType>& allFieldTypes();

struct FieldSpec {
    int firstByte = 1; // 1-based, as in the standards
    FieldType type = FieldType::Int32;
    bool bigEndian = true;
    int lastByte() const { return firstByte + fieldTypeSize(type) - 1; }
    bool valid(int headerSize = 240) const { return firstByte >= 1 && lastByte() <= headerSize; }
};

// The field's value (NaN when it lies outside the header). UInt64 values
// above 2^63 - 1 come out as large positive doubles.
double decodeField(const uint8_t* header, const FieldSpec& spec, int headerSize = 240);

// --- The fields the app reads, mapped onto bytes ---------------------------

struct MappedField {
    std::string key;  // "FFID", "CHAN", "CDP", "OFFSET", "SOU_X", ... (fixed)
    std::string name; // shown to the user
    int standardByte = 1; // where the app reads it (SEG-Y rev 1)
    int standardSize = 4; // 2 or 4 bytes
    FieldSpec spec;       // where it is in this file
};

struct HeaderMapping {
    std::vector<MappedField> fields;
    bool isStandard() const; // every field at its standard position as a BE integer
    const MappedField* find(const std::string& key) const;
    MappedField* find(const std::string& key);
};

// FFID, channel, CDP, offset, coordinate scalar, source/receiver/CDP X and
// Y, inline, crossline, shotpoint -- at their SEG-Y rev 1 positions.
HeaderMapping standardHeaderMapping();

// Template text, one field per line: "CDP = 21 4I BE". Unknown keys and
// blank or '#' lines are ignored; a field missing from the text keeps its
// standard position. False (with `error`) for a malformed line.
std::string headerMappingToText(const HeaderMapping& mapping);
bool headerMappingFromText(const std::string& text, HeaderMapping& mapping, std::string* error);

// Writes the mapped values of `src` (this file's header) into a copy of it
// at the standard positions, so everything downstream reads them there.
// Float coordinates are written x100 with coordinate scalar -100.
void normalizeTraceHeader(const uint8_t* src, uint8_t* dst240, const HeaderMapping& mapping);

// --- Header positions from the text header ---------------------------------

struct FoundField {
    std::string name;   // as written, upper case, spaces collapsed
    int firstByte = 0;  // 1-based
    int lastByte = 0;
    int line = 0;       // 1-based text line (80 columns) it came from
    std::string key;    // the mapping key it matches, empty when none
};

// Integer fields (2 or 4 bytes, within 1..240) in four notations:
//   A  "CDP_X 181-184 4I"            (name, range, optional type token)
//   B  "INLINE :BYTES 13-16"
//   C  "SRC. LINE ID(197-200)"
//   D  "BYTES 1-4=IL", "BYTES 73-76 = CDP X"
// Anything else ("FOLD: 10-13", "IL RANGE : 6504 -25432") is ignored.
// `text` is the decoded 3200-byte header, 40 lines of 80 columns.
std::vector<FoundField> findHeaderPositions(const std::string& text);

// Applies found fields to the mapping (matching keys only); returns how many.
int applyFoundFields(const std::vector<FoundField>& found, HeaderMapping& mapping);

// --- Trace selection --------------------------------------------------------

struct KeyRange {
    std::string key; // a mapping key
    double min = 0.0, max = 0.0;
    double step = 1.0; // keep values min + k * step (<= 0: every value)
};

struct TraceSelection {
    int64_t first = 0;     // 0-based, inclusive
    int64_t last = -1;     // inclusive; < 0 = to the end
    int64_t every = 1;     // every Nth trace from `first`
    std::vector<KeyRange> ranges;
    bool isAll() const { return first == 0 && last < 0 && every <= 1 && ranges.empty(); }
};

// Whether trace `index` (0-based) with mapped `header` is selected.
bool traceSelected(const TraceSelection& sel, int64_t index, const uint8_t* header, const HeaderMapping& mapping);

// --- SEG-Y layout (rev 2) ---------------------------------------------------

struct SegyLayout {
    int revisionMajor = 0, revisionMinor = 0;
    uint64_t dataStart = 3600;     // byte offset of the first trace
    int extendedHeaders = 0;       // extended text headers found
    std::vector<std::string> extendedText; // decoded, 3200 bytes each
    bool layoutSupported = true;   // false: headers only, no traces
    std::string reason;            // why not supported
    std::vector<std::string> notes;
};

// From the 3600-byte file header (and the file itself, for extended text
// headers given as -1). Rev < 2: the rev 2 fields are ignored with a note.
SegyLayout analyseLayout(const uint8_t* file, uint64_t fileSize);

// The 3200-byte text header as text: EBCDIC or ASCII, whichever reads
// better (EBCDIC on a tie). `encoding` gets "EBCDIC" or "ASCII".
std::string decodeTextBlock(const uint8_t* block3200, std::string* encoding);

} // namespace segy
