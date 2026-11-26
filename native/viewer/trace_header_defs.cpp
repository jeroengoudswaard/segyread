#include "trace_header_defs.h"

#include <cmath>
#include <cstdio>

namespace segy {

namespace {

using K = HeaderFieldKind;

// SEG-Y rev 1.0, bytes 1-240 (SEG Technical Standards Committee, 2002).
std::vector<HeaderFieldDef> rev1Fields() {
    return {
        {1, 4, K::Int, "TRACENO", "Trace sequence number within line"},
        {5, 4, K::Int, "TRACENO_FILE", "Trace sequence number within SEG Y file"},
        {9, 4, K::Int, "FFID", "Original field record number"},
        {13, 4, K::Int, "CHAN", "Trace number within the original field record"},
        {17, 4, K::Int, "SOURCE", "Energy source point number"},
        {21, 4, K::Int, "CDP", "Ensemble number (CDP, CMP, CRP, ...)"},
        {25, 4, K::Int, "SEQNO", "Trace number within the ensemble"},
        {29, 2, K::Int, "TRC_TYPE", "Trace identification code (1 seismic, 2 dead, 3 dummy, 4 time break, ...)"},
        {31, 2, K::Int, "VSTACK", "Number of vertically summed traces"},
        {33, 2, K::Int, "FOLD", "Number of horizontally stacked traces"},
        {35, 2, K::Int, "DATA_USE", "Data use (1 = production, 2 = test)"},
        {37, 4, K::Int, "OFFSET", "Distance from source point to receiver group"},
        {41, 4, K::Int, "REC_ELEV", "Receiver group elevation"},
        {45, 4, K::Int, "SOU_ELEV", "Surface elevation at source"},
        {49, 4, K::Int, "DEPTH", "Source depth below surface"},
        {53, 4, K::Int, "REC_DATUM", "Datum elevation at receiver group"},
        {57, 4, K::Int, "SOU_DATUM", "Datum elevation at source"},
        {61, 4, K::Int, "SOU_H2OD", "Water depth at source"},
        {65, 4, K::Int, "REC_H2OD", "Water depth at group"},
        {69, 2, K::Int, "ELEV_SCALAR", "Scalar for elevations and depths (bytes 41-68)"},
        {71, 2, K::Int, "COORD_SCALAR", "Scalar for coordinates (bytes 73-88)"},
        {73, 4, K::Int, "SOU_X", "Source coordinate X"},
        {77, 4, K::Int, "SOU_Y", "Source coordinate Y"},
        {81, 4, K::Int, "REC_X", "Group coordinate X"},
        {85, 4, K::Int, "REC_Y", "Group coordinate Y"},
        {89, 2, K::Int, "COORD_UNITS", "Coordinate units (1 length, 2 arc seconds, 3 decimal degrees, 4 DMS)"},
        {91, 2, K::Int, "WEATH_VEL", "Weathering velocity"},
        {93, 2, K::Int, "SUBWEATH_VEL", "Subweathering velocity"},
        {95, 2, K::Int, "UPHOLE", "Uphole time at source (ms)"},
        {97, 2, K::Int, "REC_UPHOLE", "Uphole time at group (ms)"},
        {99, 2, K::Int, "SOU_STAT", "Source static correction (ms)"},
        {101, 2, K::Int, "REC_STAT", "Group static correction (ms)"},
        {103, 2, K::Int, "TOT_STAT", "Total static applied (ms)"},
        {105, 2, K::Int, "LAG_A", "Lag time A: end of header to time break (ms)"},
        {107, 2, K::Int, "LAG_B", "Lag time B: time break to source initiation (ms)"},
        {109, 2, K::Int, "DELAY", "Delay recording time (ms)"},
        {111, 2, K::Int, "MUTE_START", "Mute time start (ms)"},
        {113, 2, K::Int, "MUTE_END", "Mute time end (ms)"},
        {115, 2, K::Int, "NUMSMP", "Number of samples in this trace"},
        {117, 2, K::Int, "DT", "Sample interval for this trace (us)"},
        {119, 2, K::Int, "GAIN_TYPE", "Gain type of field instruments (1 fixed, 2 binary, 3 floating point)"},
        {121, 2, K::Int, "GAIN_CONST", "Instrument gain constant (dB)"},
        {123, 2, K::Int, "EARLY_GAIN", "Instrument early or initial gain (dB)"},
        {125, 2, K::Int, "CORRELATED", "Correlated (1 = no, 2 = yes)"},
        {127, 2, K::Int, "SWEEP_START", "Sweep frequency at start (Hz)"},
        {129, 2, K::Int, "SWEEP_END", "Sweep frequency at end (Hz)"},
        {131, 2, K::Int, "SWEEP_LEN", "Sweep length (ms)"},
        {133, 2, K::Int, "SWEEP_TYPE", "Sweep type (1 linear, 2 parabolic, 3 exponential, 4 other)"},
        {135, 2, K::Int, "SWEEP_TAPER_START", "Sweep trace taper length at start (ms)"},
        {137, 2, K::Int, "SWEEP_TAPER_END", "Sweep trace taper length at end (ms)"},
        {139, 2, K::Int, "TAPER_TYPE", "Taper type (1 linear, 2 cosine squared, 3 other)"},
        {141, 2, K::Int, "ALIAS_FREQ", "Alias filter frequency (Hz)"},
        {143, 2, K::Int, "ALIAS_SLOPE", "Alias filter slope (dB/octave)"},
        {145, 2, K::Int, "NOTCH_FREQ", "Notch filter frequency (Hz)"},
        {147, 2, K::Int, "NOTCH_SLOPE", "Notch filter slope (dB/octave)"},
        {149, 2, K::Int, "LOWCUT_FREQ", "Low-cut frequency (Hz)"},
        {151, 2, K::Int, "HIGHCUT_FREQ", "High-cut frequency (Hz)"},
        {153, 2, K::Int, "LOWCUT_SLOPE", "Low-cut slope (dB/octave)"},
        {155, 2, K::Int, "HIGHCUT_SLOPE", "High-cut slope (dB/octave)"},
        {157, 2, K::Int, "YEAR", "Year data recorded"},
        {159, 2, K::Int, "DAY", "Day of year"},
        {161, 2, K::Int, "HOUR", "Hour of day (24-hour clock)"},
        {163, 2, K::Int, "MINUTE", "Minute of hour"},
        {165, 2, K::Int, "SECOND", "Second of minute"},
        {167, 2, K::Int, "TIME_BASIS", "Time basis code (1 local, 2 GMT, 3 other, 4 UTC)"},
        {169, 2, K::Int, "WEIGHT", "Trace weighting factor (2^-N volts per least significant bit)"},
        {171, 2, K::Int, "GEO_ROLL1", "Geophone group number of roll switch position one"},
        {173, 2, K::Int, "GEO_FIRST", "Geophone group number of first trace of original field record"},
        {175, 2, K::Int, "GEO_LAST", "Geophone group number of last trace of original field record"},
        {177, 2, K::Int, "GAP", "Gap size (total number of groups dropped)"},
        {179, 2, K::Int, "OVERTRAVEL", "Over travel associated with taper (1 down/behind, 2 up/ahead)"},
        {181, 4, K::Int, "CDP_X", "X coordinate of ensemble (CDP) position"},
        {185, 4, K::Int, "CDP_Y", "Y coordinate of ensemble (CDP) position"},
        {189, 4, K::Int, "ILINE_NO", "In-line number (3D)"},
        {193, 4, K::Int, "XLINE_NO", "Cross-line number (3D)"},
        {197, 4, K::Int, "SP", "Shotpoint number"},
        {201, 2, K::Int, "SP_SCALAR", "Scalar to be applied to the shotpoint number"},
        {203, 2, K::Int, "TRACE_UNIT", "Trace value measurement unit"},
        {205, 6, K::MantissaExponent, "TRANSD_CONST", "Transduction constant (mantissa x 10^exponent)"},
        {211, 2, K::Int, "TRANSD_UNIT", "Transduction units"},
        {213, 2, K::Int, "DEVICE_ID", "Device/trace identifier"},
        {215, 2, K::Int, "TIME_SCALAR", "Scalar to be applied to times in bytes 95-114"},
        {217, 2, K::Int, "SOURCE_TYPE", "Source type/orientation"},
        {219, 6, K::ThreeShorts, "SOURCE_DIR", "Source energy direction (vertical, cross-line, in-line)"},
        {225, 6, K::MantissaExponent, "SOURCE_MEAS", "Source measurement (mantissa x 10^exponent)"},
        {231, 2, K::Int, "SOURCE_UNIT", "Source measurement unit"},
        {233, 8, K::Unassigned, "", "Unassigned -- for optional information"},
    };
}

// SEG-Y rev 2.0 (2017): the same standard header, with the last eight bytes
// either binary zeros or the trace header name "SEG00000", and the trace
// identification code list extended.
std::vector<HeaderFieldDef> rev2Fields() {
    std::vector<HeaderFieldDef> fields = rev1Fields();
    for (HeaderFieldDef& f : fields) {
        if (f.firstByte == 29) {
            f.description = "Trace identification code (rev 2.0 list: 1 seismic, 2 dead, ..., 41-43 multi-component, ...)";
        }
        if (f.firstByte == 233) {
            f.kind = K::Ascii;
            f.name = "HEADER_NAME";
            f.description = "Trace header name: \"SEG00000\" or binary zeros";
        }
    }
    return fields;
}

// Shell "SEGY trace identing" (V2.1), bytes 1-240, from the user's document.
std::vector<HeaderFieldDef> shellFields() {
    return {
        {1, 4, K::Int, "TRACENO", "Trace sequence number within a line (first trace of each line is 1)"},
        {5, 4, K::Int, "TRACENO_REEL", "Trace number within the reel"},
        {9, 4, K::Int, "FFID", "Original field record number"},
        {13, 4, K::Int, "CHAN", "Trace sequence number within original field record"},
        {17, 4, K::Int, "SP", "Shot point number (acquisition)"},
        {21, 4, K::Int, "CDP", "CDP number (2D) / composite CDP or crossline number (3D)"},
        {25, 4, K::Int, "SEQNO", "Trace number within the CDP ensemble (starts at one)"},
        {29, 2, K::Int, "TRC_TYPE", "Trace identification code (1 seismic, 2 dead, 3 dummy, ...; 111-120 noisy-trace grade)"},
        {31, 2, K::Int, "VSTACK", "Number of vertically summed traces yielding this one"},
        {33, 2, K::Int, "FOLD", "Number of horizontally stacked traces (bin multiplicity post-stack)"},
        {35, 2, K::Int, "DATA_USE", "Data use (1 = production, 2 = test)"},
        {37, 4, K::Int, "OFFSET", "Distance from source point to receiver group"},
        {41, 4, K::Int, "TIDAL_STAT", "Tidal static correction x 100 (ms)"},
        {45, 4, K::Unassigned, "", "Not used"},
        {49, 4, K::Int, "SOU_DEPTH", "Source depth below mean sea level (positive)"},
        {53, 4, K::Int, "REC_DEPTH", "Receiver group depth below mean sea level (positive)"},
        {57, 4, K::Int, "SEABED_TWT", "Seabed reflection time (TWT) at CDP; gridded post-stack"},
        {61, 4, K::Int, "SOU_H2OD", "Water depth at source"},
        {65, 4, K::Int, "REC_H2OD", "Water depth at group"},
        {69, 2, K::Int, "ELEV_SCALAR", "Scalar for elevations and depths in bytes 41-68 (+ multiply, - divide)"},
        {71, 2, K::Int, "COORD_SCALAR", "Scalar for coordinates in bytes 73-88 (preferred -10: decimetres)"},
        {73, 4, K::Int, "SOU_X", "X coordinate of the source centre"},
        {77, 4, K::Int, "SOU_Y", "Y coordinate of the source centre"},
        {81, 4, K::Int, "REC_X", "X coordinate of the receiver centre"},
        {85, 4, K::Int, "REC_Y", "Y coordinate of the receiver centre"},
        {89, 2, K::Int, "COORD_UNITS", "Coordinate units (1 length, 2 arc seconds, 3 decimal degrees, 4 DMS)"},
        {91, 2, K::Int, "WATER_VEL_SURF", "Near-surface water velocity for gun and cable correction"},
        {93, 2, K::Int, "WATER_VEL_AVG", "Average velocity over the water layer"},
        {95, 2, K::Unassigned, "", "Not used"},
        {97, 2, K::Unassigned, "", "Not used"},
        {99, 2, K::Int, "SOU_STAT", "Source residual static (scalar in bytes 217-218)"},
        {101, 2, K::Int, "REC_STAT", "Group residual static (scalar in bytes 217-218)"},
        {103, 2, K::Int, "TOT_STAT", "Total static applied (scalar in bytes 217-218)"},
        {105, 2, K::Int, "LAG_A", "Lag time A: end of header to time break (ms)"},
        {107, 2, K::Int, "LAG_B", "Lag time B: time break to source initiation (ms)"},
        {109, 2, K::Int, "DELAY", "Delay recording time (ms)"},
        {111, 2, K::Int, "MUTE_START", "Mute time start"},
        {113, 2, K::Int, "MUTE_END", "Mute time end"},
        {115, 2, K::Int, "NUMSMP", "Number of samples in this trace"},
        {117, 2, K::Int, "DT", "Sample interval for this trace (us)"},
        {119, 2, K::Int, "SURVEY_ID", "Survey acquisition identifier (E-project code)"},
        {121, 2, K::Int, "PROC_ID", "Processing identifier (E-project code)"},
        {123, 2, K::Int, "FIRST_BREAK", "First break pick (scalar in bytes 217-218)"},
        {125, 4, K::Unassigned, "", "Not used"},
        {129, 4, K::Int, "SOU_STATION", "Processing contractor's internal source point station number"},
        {133, 4, K::Int, "REC_STATION", "Processing contractor's internal receiver group station number"},
        {137, 2, K::Unassigned, "", "Not used"},
        {139, 2, K::Int, "REC_INDEX", "Receiver index (e.g. multicomponent)"},
        {141, 2, K::Int, "SRCREC_AMP_SCALAR", "Scalar for the amplitude scalar in bytes 143-144"},
        {143, 2, K::Int, "SRCREC_AMP", "Source-type into receiver-type amplitude scalar"},
        {145, 2, K::Int, "CHAN_AMP_SCALAR", "Scalar for the channel-consistent amplitude scalars in bytes 147-148"},
        {147, 2, K::Int, "SHOT_AMP", "Shot-consistent amplitude scalar"},
        {149, 2, K::Int, "CHAN_AMP", "Channel-consistent amplitude scalar"},
        {151, 2, K::Int, "CMP_AMP", "CMP/bin surface-consistent amplitude scalar x 1000"},
        {153, 2, K::Int, "SURVEY_AMP", "Survey-consistent long-wavelength offset amplitude scalar x 1000"},
        {155, 2, K::Int, "SWATH_AMP", "Swath-consistent short-wavelength channel amplitude scalar x 1000"},
        {157, 2, K::Int, "YEAR", "Year data recorded (4 digits)"},
        {159, 2, K::Int, "DAY", "Day of year (Julian day for GMT and UTC)"},
        {161, 2, K::Int, "HOUR", "Hour of day (24-hour clock)"},
        {163, 2, K::Int, "MINUTE", "Minute of hour"},
        {165, 2, K::Int, "SECOND", "Second of minute"},
        {167, 2, K::Int, "TIME_BASIS", "Time basis code (1 local, 2 GMT, 3 other, 4 UTC)"},
        {169, 2, K::Int, "WEIGHT", "Trace weighting factor (2^-N volts per least significant bit)"},
        {171, 2, K::Int, "GUN_VOLUME", "Gun volume (cubic inches)"},
        {173, 2, K::Unassigned, "", "Not used"},
        {175, 2, K::Unassigned, "", "Not used"},
        {177, 2, K::Int, "GUN_VOL_SCALAR", "Gun volume scalar factor"},
        {179, 2, K::Int, "GUN_VOL_SCALE", "Gun volume scalar (x 10000)"},
        {181, 4, K::Int, "CDP_X", "X coordinate of CDP/bin centre"},
        {185, 4, K::Int, "CDP_Y", "Y coordinate of CDP/bin centre"},
        {189, 4, K::Int, "ILINE_NO", "Inline number"},
        {193, 4, K::Int, "XLINE_NO", "Crossline number"},
        {197, 2, K::Int, "CABLE_ID", "Cable identification number"},
        {199, 2, K::Unassigned, "", "Not used"},
        {201, 2, K::Int, "SOURCE_TYPE", "Source type (codes annotated in the EBCDIC header)"},
        {203, 2, K::Int, "RECEIVER_TYPE", "Receiver/instrument type (codes annotated in the EBCDIC header)"},
        {205, 2, K::Int, "GUN_MASK", "Gun mask (corrected, after seismic and navigation merge QC)"},
        {207, 2, K::Int, "SAIL_SEQ", "Sailed line sequence number"},
        {209, 4, K::Unassigned, "", "Not used"},
        {213, 4, K::Unassigned, "", "Not used"},
        {217, 2, K::Int, "STATIC_SCALAR", "Scale factor for statics (0/1 ms, 10: 0.1 ms, 100: 0.01 ms, 1000: 0.001 ms)"},
        {219, 2, K::Int, "GUN_CORR", "Gun correction to MSL (scalar in bytes 217-218)"},
        {221, 2, K::Int, "CABLE_CORR", "Cable correction to MSL (scalar in bytes 217-218)"},
        {223, 2, K::Int, "EDIT_FLAG", "Edit flag (-1 killed, 1 ok, 2 to be killed, -3/3 polarity reversal, 4 auto kill)"},
        {225, 2, K::Int, "GUN_PRESSURE", "Air gun pressure"},
        {227, 2, K::Int, "BOAT_ID", "Boat identifier (multi-boat operations)"},
        {229, 2, K::Int, "DATA_START", "Start of data"},
        {231, 2, K::Int, "DATA_END", "End of data"},
        {233, 2, K::Int, "REC_DEPTH_SL", "Depth of receiver group below sea level (positive)"},
        {235, 4, K::Int, "SAIL_LINE", "Sailed line number (numerical part)"},
        {239, 2, K::Int, "TEST_SEQ", "Processing test sequence number"},
    };
}

} // namespace

const std::vector<HeaderFieldDef>& traceHeaderDefinition(HeaderStandard standard) {
    static const std::vector<HeaderFieldDef> shell = shellFields();
    static const std::vector<HeaderFieldDef> rev1 = rev1Fields();
    static const std::vector<HeaderFieldDef> rev2 = rev2Fields();
    switch (standard) {
        case HeaderStandard::SegyRev1: return rev1;
        case HeaderStandard::SegyRev2: return rev2;
        case HeaderStandard::ShellV21: break;
    }
    return shell;
}

const char* headerStandardName(HeaderStandard standard) {
    switch (standard) {
        case HeaderStandard::SegyRev1: return "SEG-Y rev 1.0";
        case HeaderStandard::SegyRev2: return "SEG-Y rev 2.0";
        case HeaderStandard::ShellV21: break;
    }
    return "SEG-Y v2.1";
}

int64_t readHeaderInt(const uint8_t* header, int firstByte, int size, bool bigEndian) {
    const uint8_t* p = header + (firstByte - 1);
    uint32_t raw = 0;
    for (int i = 0; i < size; ++i) {
        const uint8_t byte = bigEndian ? p[i] : p[size - 1 - i];
        raw = (raw << 8) | byte;
    }
    if (size == 1) return int8_t(raw);
    if (size == 2) return int16_t(uint16_t(raw));
    return int32_t(raw);
}

std::string formatHeaderField(const uint8_t* header, const HeaderFieldDef& field, bool bigEndian) {
    char buffer[64];
    switch (field.kind) {
        case K::Int:
            return std::to_string(readHeaderInt(header, field.firstByte, field.size, bigEndian));
        case K::MantissaExponent: {
            const int64_t mantissa = readHeaderInt(header, field.firstByte, 4, bigEndian);
            const int64_t exponent = readHeaderInt(header, field.firstByte + 4, 2, bigEndian);
            if (exponent == 0) return std::to_string(mantissa);
            std::snprintf(buffer, sizeof buffer, "%lldE%lld", static_cast<long long>(mantissa),
                          static_cast<long long>(exponent));
            return buffer;
        }
        case K::ThreeShorts: {
            std::snprintf(buffer, sizeof buffer, "%lld, %lld, %lld",
                          static_cast<long long>(readHeaderInt(header, field.firstByte, 2, bigEndian)),
                          static_cast<long long>(readHeaderInt(header, field.firstByte + 2, 2, bigEndian)),
                          static_cast<long long>(readHeaderInt(header, field.firstByte + 4, 2, bigEndian)));
            return buffer;
        }
        case K::Ascii: {
            std::string text;
            bool allZero = true;
            for (int i = 0; i < field.size; ++i) {
                const uint8_t c = header[field.firstByte - 1 + i];
                allZero = allZero && c == 0;
                text += (c >= 32 && c < 127) ? char(c) : '.';
            }
            return allZero ? std::string("0") : text;
        }
        case K::Unassigned: {
            bool allZero = true;
            std::string hex;
            for (int i = 0; i < field.size; ++i) {
                const uint8_t c = header[field.firstByte - 1 + i];
                allZero = allZero && c == 0;
                std::snprintf(buffer, sizeof buffer, "%02X", c);
                hex += buffer;
            }
            return allZero ? std::string("0") : hex;
        }
    }
    return "";
}

} // namespace segy
