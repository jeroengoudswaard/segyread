#include "header_fields.h"

#include "segy_decode.h"
#include "segy_format.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <regex>
#include <sstream>

namespace segy {

int fieldTypeSize(FieldType type) {
    switch (type) {
    case FieldType::Int16:
    case FieldType::UInt16: return 2;
    case FieldType::Int64:
    case FieldType::UInt64:
    case FieldType::Ieee64: return 8;
    default: return 4;
    }
}

const char* fieldTypeName(FieldType type) {
    switch (type) {
    case FieldType::Int16: return "2I";
    case FieldType::Int32: return "4I";
    case FieldType::UInt16: return "2U";
    case FieldType::UInt32: return "4U";
    case FieldType::Ieee32: return "4F";
    case FieldType::Ibm32: return "4B";
    case FieldType::Int64: return "8I";
    case FieldType::UInt64: return "8U";
    case FieldType::Ieee64: return "8F";
    }
    return "4I";
}

const std::vector<FieldType>& allFieldTypes() {
    static const std::vector<FieldType> types = {FieldType::Int16,  FieldType::Int32, FieldType::UInt16,
                                                 FieldType::UInt32, FieldType::Ieee32, FieldType::Ibm32,
                                                 FieldType::Int64,  FieldType::UInt64, FieldType::Ieee64};
    return types;
}

bool parseFieldType(const std::string& text, FieldType& out) {
    std::string t;
    for (char c : text) t += char(std::toupper(static_cast<unsigned char>(c)));
    for (FieldType type : allFieldTypes()) {
        if (t == fieldTypeName(type)) {
            out = type;
            return true;
        }
    }
    return false;
}

double decodeField(const uint8_t* header, const FieldSpec& spec, int headerSize) {
    if (!spec.valid(headerSize)) return std::numeric_limits<double>::quiet_NaN();
    const int n = fieldTypeSize(spec.type);
    uint8_t b[8] = {};
    // Into big-endian order first; every type is decoded from that.
    for (int i = 0; i < n; ++i) b[i] = header[spec.firstByte - 1 + (spec.bigEndian ? i : n - 1 - i)];
    uint64_t u = 0;
    for (int i = 0; i < n; ++i) u = (u << 8) | b[i];
    switch (spec.type) {
    case FieldType::Int16: return double(int16_t(uint16_t(u)));
    case FieldType::UInt16: return double(uint16_t(u));
    case FieldType::Int32: return double(int32_t(uint32_t(u)));
    case FieldType::UInt32: return double(uint32_t(u));
    case FieldType::Int64: return double(int64_t(u));
    case FieldType::UInt64: return double(u);
    case FieldType::Ieee32: {
        const uint32_t bits = uint32_t(u);
        float f;
        std::memcpy(&f, &bits, 4);
        return double(f);
    }
    case FieldType::Ibm32: return double(ibmToIeeeScalar(uint32_t(u)));
    case FieldType::Ieee64: {
        double d;
        std::memcpy(&d, &u, 8);
        return d;
    }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

// --- Mapping -----------------------------------------------------------------

namespace {
struct StandardField {
    const char* key;
    const char* name;
    int byte;
    int size;
};
const StandardField kStandardFields[] = {
    {"FFID", "Field record (FFID)", 9, 4},   {"CHAN", "Channel", 13, 4},
    {"SP", "Shotpoint (source point)", 17, 4}, {"CDP", "CDP", 21, 4},
    {"OFFSET", "Offset", 37, 4},              {"COORD_SCALAR", "Coordinate scalar", 71, 2},
    {"SOU_X", "Source X", 73, 4},             {"SOU_Y", "Source Y", 77, 4},
    {"REC_X", "Receiver X", 81, 4},           {"REC_Y", "Receiver Y", 85, 4},
    {"CDP_X", "CDP X", 181, 4},               {"CDP_Y", "CDP Y", 185, 4},
    {"INLINE", "Inline", 189, 4},             {"XLINE", "Crossline", 193, 4},
    {"SHOTPOINT", "Shotpoint (rev 1)", 197, 4},
};

bool isCoordinateKey(const std::string& key) {
    return key == "SOU_X" || key == "SOU_Y" || key == "REC_X" || key == "REC_Y" || key == "CDP_X" || key == "CDP_Y";
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

HeaderMapping standardHeaderMapping() {
    HeaderMapping m;
    for (const StandardField& f : kStandardFields) {
        MappedField mf;
        mf.key = f.key;
        mf.name = f.name;
        mf.standardByte = f.byte;
        mf.standardSize = f.size;
        mf.spec.firstByte = f.byte;
        mf.spec.type = f.size == 2 ? FieldType::Int16 : FieldType::Int32;
        mf.spec.bigEndian = true;
        m.fields.push_back(mf);
    }
    return m;
}

bool HeaderMapping::isStandard() const {
    for (const MappedField& f : fields) {
        const FieldType want = f.standardSize == 2 ? FieldType::Int16 : FieldType::Int32;
        if (f.spec.firstByte != f.standardByte || f.spec.type != want || !f.spec.bigEndian) return false;
    }
    return true;
}

const MappedField* HeaderMapping::find(const std::string& key) const {
    for (const MappedField& f : fields) {
        if (f.key == key) return &f;
    }
    return nullptr;
}

MappedField* HeaderMapping::find(const std::string& key) {
    for (MappedField& f : fields) {
        if (f.key == key) return &f;
    }
    return nullptr;
}

std::string headerMappingToText(const HeaderMapping& mapping) {
    std::ostringstream out;
    out << "# TraceLab trace-header mapping: KEY = first byte (1-based), type, byte order\n";
    out << "# Types: 2I 4I 2U 4U 4F (IEEE) 4B (IBM) 8I 8U 8F\n";
    for (const MappedField& f : mapping.fields) {
        out << f.key << " = " << f.spec.firstByte << ' ' << fieldTypeName(f.spec.type) << ' '
            << (f.spec.bigEndian ? "BE" : "LE") << '\n';
    }
    return out.str();
}

bool headerMappingFromText(const std::string& text, HeaderMapping& mapping, std::string* error) {
    HeaderMapping result = standardHeaderMapping();
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            if (error) *error = "Line " + std::to_string(lineNo) + ": expected KEY = byte type order";
            return false;
        }
        const std::string key = upper(trim(line.substr(0, eq)));
        std::istringstream rest(line.substr(eq + 1));
        int firstByte = 0;
        std::string typeText = "4I", order = "BE";
        if (!(rest >> firstByte)) {
            if (error) *error = "Line " + std::to_string(lineNo) + ": no byte position";
            return false;
        }
        rest >> typeText >> order;
        FieldSpec spec;
        spec.firstByte = firstByte;
        if (!parseFieldType(typeText, spec.type)) {
            if (error) *error = "Line " + std::to_string(lineNo) + ": unknown type " + typeText;
            return false;
        }
        spec.bigEndian = upper(order) != "LE";
        if (!spec.valid()) {
            if (error) *error = "Line " + std::to_string(lineNo) + ": field outside bytes 1-240";
            return false;
        }
        if (MappedField* f = result.find(key)) f->spec = spec; // unknown keys are ignored
    }
    mapping = std::move(result);
    return true;
}

void normalizeTraceHeader(const uint8_t* src, uint8_t* dst240, const HeaderMapping& mapping) {
    std::memcpy(dst240, src, 240);
    bool floatCoordinates = false;
    for (const MappedField& f : mapping.fields) {
        if (isCoordinateKey(f.key) &&
            (f.spec.type == FieldType::Ieee32 || f.spec.type == FieldType::Ibm32 || f.spec.type == FieldType::Ieee64)) {
            floatCoordinates = true;
        }
    }
    // Read everything first: a field may move onto another's old position.
    std::vector<double> values;
    for (const MappedField& f : mapping.fields) values.push_back(decodeField(src, f.spec));
    for (size_t i = 0; i < mapping.fields.size(); ++i) {
        const MappedField& f = mapping.fields[i];
        double v = values[i];
        if (!std::isfinite(v)) v = 0.0;
        if (floatCoordinates && isCoordinateKey(f.key)) v *= 100.0;
        if (floatCoordinates && f.key == "COORD_SCALAR") v = -100.0;
        uint8_t* p = dst240 + f.standardByte - 1;
        if (f.standardSize == 2) {
            writeI16BE(p, int16_t(std::clamp(std::llround(v), -32768LL, 32767LL)));
        } else {
            writeI32BE(p, int32_t(std::clamp(std::llround(v), -2147483648LL, 2147483647LL)));
        }
    }
}

// --- Header positions from the text header ------------------------------------

namespace {
// Name -> mapping key, from the spellings seen in text headers.
std::string keyForName(const std::string& rawName) {
    std::string n;
    for (char c : upper(rawName)) {
        if (std::isalnum(static_cast<unsigned char>(c))) n += c;
    }
    static const std::vector<std::pair<std::vector<const char*>, const char*>> kSynonyms = {
        {{"FFID", "FIELDRECORD", "FIELDRECORDNUMBER", "FLDR", "SHOTNUMBER", "RECORD", "FFN"}, "FFID"},
        {{"CHAN", "CHANNEL", "CHANNELNUMBER", "TRACF", "TRACENUMBER"}, "CHAN"},
        {{"CDP", "CMP", "CDPNUMBER", "CMPNUMBER", "ENSEMBLE", "ENSEMBLENUMBER", "BIN"}, "CDP"},
        {{"OFFSET", "OFF", "SOURCERECEIVEROFFSET"}, "OFFSET"},
        {{"COORDSCALAR", "SCALCO", "COORDINATESCALAR", "SCALAR"}, "COORD_SCALAR"},
        {{"SOUX", "SRCX", "SX", "SOURCEX", "SHOTX"}, "SOU_X"},
        {{"SOUY", "SRCY", "SY", "SOURCEY", "SHOTY"}, "SOU_Y"},
        {{"RECX", "GX", "RECEIVERX", "GROUPX"}, "REC_X"},
        {{"RECY", "GY", "RECEIVERY", "GROUPY"}, "REC_Y"},
        {{"CDPX", "CMPX", "XCDP", "MIDPOINTX", "ENSEMBLEX", "X"}, "CDP_X"},
        {{"CDPY", "CMPY", "YCDP", "MIDPOINTY", "ENSEMBLEY", "Y"}, "CDP_Y"},
        {{"INLINE", "IL", "INL", "ILINE", "INLINENUMBER", "LINE"}, "INLINE"},
        {{"XLINE", "XL", "CROSSLINE", "CRL", "XLINENUMBER", "CROSSLINENUMBER"}, "XLINE"},
        {{"SP", "SHOTPOINT", "SHOTPOINTNUMBER", "SOURCEPOINT"}, "SHOTPOINT"},
    };
    for (const auto& entry : kSynonyms) {
        for (const char* s : entry.first) {
            if (n == s) return entry.second;
        }
    }
    return std::string();
}

std::string cleanName(const std::string& raw) {
    std::string out;
    bool space = false;
    for (char c : upper(trim(raw))) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = true;
            continue;
        }
        if (space && !out.empty()) out += ' ';
        space = false;
        out += c;
    }
    // Trailing separators: "CDP X :" -> "CDP X".
    while (!out.empty() && (out.back() == ':' || out.back() == '=' || out.back() == '-' || out.back() == ' ')) out.pop_back();
    return out;
}

bool plausible(int a, int b) {
    const int w = b - a + 1;
    return a >= 1 && b <= 240 && (w == 2 || w == 4);
}
} // namespace

std::vector<FoundField> findHeaderPositions(const std::string& text) {
    std::vector<FoundField> found;
    const std::string name = R"(([A-Za-z][A-Za-z0-9_ .\-/]{0,23}?))";
    // A: NAME 181-184 [4I]   (also "NAME: 181-184")
    const std::regex tableRe(name + R"(\s*:?\s+(\d{1,3})\s*-\s*(\d{1,3})(?:\s+(\d[IiFf]))?(?=\s|\||$))");
    // B: NAME :BYTES 13-16
    const std::regex bytesRe(name + R"(\s*:\s*BYTES?\s*(\d{1,3})\s*-\s*(\d{1,3}))", std::regex::icase);
    // C: NAME(197-200)
    const std::regex parenRe(name + R"(\s*\(\s*(\d{1,3})\s*-\s*(\d{1,3})\s*\))");
    // D: BYTES 1-4=IL
    const std::regex equalsRe(R"(BYTES?\s*(\d{1,3})\s*-\s*(\d{1,3})\s*=\s*([A-Za-z][A-Za-z0-9_ .]{0,23}?)(?=\s{2,}|,|;|\||$))",
                              std::regex::icase);
    const size_t lines = (text.size() + 79) / 80;
    for (size_t li = 0; li < lines; ++li) {
        std::string line = text.substr(li * 80, 80);
        // Card numbers "C 1 " / "C01" at the start are not names.
        line = std::regex_replace(line, std::regex(R"(^C\s?\d{1,2}\s)"), "    ");
        auto add = [&](const std::string& rawName, int a, int b) {
            const std::string n = cleanName(rawName);
            if (n.empty() || !plausible(a, b)) return;
            // "IL RANGE", "FOLD" etc. stay out: only names, not ranges of values.
            if (n.find("RANGE") != std::string::npos) return;
            for (const FoundField& f : found) {
                if (f.firstByte == a && f.name == n) return;
            }
            FoundField f;
            f.name = n;
            f.firstByte = a;
            f.lastByte = b;
            f.line = int(li) + 1;
            f.key = keyForName(n);
            found.push_back(f);
        };
        std::smatch m;
        std::string rest = line;
        bool any = false;
        for (std::sregex_iterator it(line.begin(), line.end(), equalsRe), end; it != end; ++it) {
            add((*it)[3].str(), std::stoi((*it)[1].str()), std::stoi((*it)[2].str()));
            any = true;
        }
        if (any) continue;
        for (std::sregex_iterator it(line.begin(), line.end(), bytesRe), end; it != end; ++it) {
            add((*it)[1].str(), std::stoi((*it)[2].str()), std::stoi((*it)[3].str()));
            any = true;
        }
        if (any) continue;
        for (std::sregex_iterator it(line.begin(), line.end(), parenRe), end; it != end; ++it) {
            add((*it)[1].str(), std::stoi((*it)[2].str()), std::stoi((*it)[3].str()));
            any = true;
        }
        if (any) continue;
        // A: the colon form "FOLD: 10-13" is a value, not a position -- only
        // the plain "NAME 181-184" table form counts.
        for (std::sregex_iterator it(line.begin(), line.end(), tableRe), end; it != end; ++it) {
            const std::string whole = (*it)[0].str();
            if (whole.find(':') != std::string::npos) continue;
            add((*it)[1].str(), std::stoi((*it)[2].str()), std::stoi((*it)[3].str()));
        }
    }
    return found;
}

int applyFoundFields(const std::vector<FoundField>& found, HeaderMapping& mapping) {
    int applied = 0;
    for (const FoundField& f : found) {
        if (f.key.empty()) continue;
        MappedField* m = mapping.find(f.key);
        if (!m) continue;
        m->spec.firstByte = f.firstByte;
        m->spec.type = f.lastByte - f.firstByte + 1 == 2 ? FieldType::Int16 : FieldType::Int32;
        m->spec.bigEndian = true;
        ++applied;
    }
    return applied;
}

// --- Selection -----------------------------------------------------------------

bool traceSelected(const TraceSelection& sel, int64_t index, const uint8_t* header, const HeaderMapping& mapping) {
    if (index < sel.first) return false;
    if (sel.last >= 0 && index > sel.last) return false;
    if (sel.every > 1 && (index - sel.first) % sel.every != 0) return false;
    for (const KeyRange& r : sel.ranges) {
        const MappedField* f = mapping.find(r.key);
        if (!f) continue;
        const double v = decodeField(header, f->spec);
        if (!std::isfinite(v) || v < r.min - 1e-9 || v > r.max + 1e-9) return false;
        if (r.step > 0.0) {
            const double k = (v - r.min) / r.step;
            if (std::fabs(k - std::round(k)) > 1e-6) return false;
        }
    }
    return true;
}

// --- Layout --------------------------------------------------------------------

std::string decodeTextBlock(const uint8_t* block3200, std::string* encoding) {
    size_t asciiAlnum = 0, ebcdicAlnum = 0;
    for (size_t i = 0; i < 3200; ++i) {
        const uint8_t b = block3200[i];
        if ((b >= '0' && b <= '9') || (b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z')) ++asciiAlnum;
        if ((b >= 0xC1 && b <= 0xC9) || (b >= 0xD1 && b <= 0xD9) || (b >= 0xE2 && b <= 0xE9) || (b >= 0xF0 && b <= 0xF9)) {
            ++ebcdicAlnum;
        }
    }
    if (encoding) *encoding = asciiAlnum > ebcdicAlnum ? "ASCII" : "EBCDIC";
    return decodeEbcdicText(block3200); // same rule inside
}

SegyLayout analyseLayout(const uint8_t* file, uint64_t fileSize) {
    SegyLayout layout;
    if (fileSize < 3600) {
        layout.layoutSupported = false;
        layout.reason = "the file is shorter than the 3600-byte SEG-Y header";
        return layout;
    }
    const uint8_t* bin = file + 3200;
    layout.revisionMajor = bin[300]; // bytes 3501-3502: major, minor
    layout.revisionMinor = bin[301];
    const int16_t extended = readI16BE(bin + 304); // 3505-3506
    const uint64_t firstTrace = (uint64_t(readU32BE(bin + 320)) << 32) | readU32BE(bin + 324); // 3521-3528
    const int16_t additional = readI16BE(bin + 306); // 3507-3508
    if (layout.revisionMajor < 2) {
        if (firstTrace != 0) layout.notes.push_back("Byte offset of the first trace is set, but revision < 2 predates that field; ignored");
        if (additional != 0) layout.notes.push_back("Additional trace headers are set, but revision < 2 predates that field; ignored");
    }
    if (layout.revisionMajor >= 2 && firstTrace != 0) {
        layout.dataStart = firstTrace;
        layout.notes.push_back("Data start from the binary header (bytes 3521-3528): " + std::to_string(firstTrace));
    } else if (extended > 0 && layout.revisionMajor >= 1) {
        layout.dataStart = 3600 + 3200ull * uint64_t(extended);
        layout.extendedHeaders = extended;
    } else if (extended == -1 && layout.revisionMajor >= 2) {
        // Variable count: up to the block holding ((SEG:EndText)).
        uint64_t pos = 3600;
        int count = 0;
        bool endFound = false;
        while (pos + 3200 <= fileSize && count < 1000) {
            std::string block = decodeTextBlock(file + pos, nullptr);
            ++count;
            pos += 3200;
            std::string compact;
            for (char c : block.substr(0, 512)) {
                if (c != ' ') compact += char(std::toupper(static_cast<unsigned char>(c)));
            }
            if (compact.find("((SEG:ENDTEXT))") != std::string::npos) {
                endFound = true;
                break;
            }
        }
        if (!endFound) {
            layout.layoutSupported = false;
            layout.reason = "extended text headers given as -1 but no ((SEG:EndText)) block was found";
        }
        layout.dataStart = pos;
        layout.extendedHeaders = count;
    } else if (extended < 0 && layout.revisionMajor < 2) {
        layout.notes.push_back("Extended text header count " + std::to_string(extended) + " with revision < 2; ignored");
    } else if (extended < 0) {
        layout.layoutSupported = false;
        layout.reason = "unsupported extended text header count " + std::to_string(extended);
    }
    if (layout.revisionMajor >= 2 && additional > 0) {
        layout.layoutSupported = false;
        layout.reason = std::to_string(additional) + " additional trace headers per trace are not supported";
    }
    if (layout.dataStart < 3600 || layout.dataStart > fileSize) {
        layout.layoutSupported = false;
        layout.reason = "the data start (" + std::to_string(layout.dataStart) + ") lies " +
                        (layout.dataStart < 3600 ? "inside the file header" : "beyond the end of the file");
    }
    for (int i = 0; i < layout.extendedHeaders; ++i) {
        const uint64_t at = 3600 + 3200ull * uint64_t(i);
        if (at + 3200 > fileSize) break;
        layout.extendedText.push_back(decodeTextBlock(file + at, nullptr));
    }
    if (layout.extendedHeaders > 0) {
        layout.notes.push_back(std::to_string(layout.extendedHeaders) + " extended text header(s); traces start at byte " +
                               std::to_string(layout.dataStart));
    }
    return layout;
}

} // namespace segy
