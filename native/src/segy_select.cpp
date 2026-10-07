#include "segy_select.h"

#include <algorithm>

namespace segy {

const std::vector<int32_t>& TraceHeaderColumns::column(IdentField field) const {
    switch (field) {
        case IdentField::TraceSequenceLine: return traceSequenceLine;
        case IdentField::TraceSequenceFile: return traceSequenceFile;
        case IdentField::FieldRecord: return fieldRecord;
        case IdentField::TraceNumber: return traceNumber;
        case IdentField::Cdp: return cdp;
        case IdentField::X: return x;
        case IdentField::Y: return y;
    }
    return traceSequenceLine; // unreachable; silences -Wreturn-type
}

TraceHeaderColumns scanTraceHeaders(const uint8_t* fileBase, int64_t traceCount, size_t traceStrideBytes,
                                     ThreadPool& pool) {
    TraceHeaderColumns columns;
    if (traceCount <= 0) return columns;

    columns.traceSequenceLine.resize(size_t(traceCount));
    columns.traceSequenceFile.resize(size_t(traceCount));
    columns.fieldRecord.resize(size_t(traceCount));
    columns.traceNumber.resize(size_t(traceCount));
    columns.cdp.resize(size_t(traceCount));
    columns.x.resize(size_t(traceCount));
    columns.y.resize(size_t(traceCount));

    // Each worker owns a disjoint, contiguous range of trace indices -- same
    // no-synchronization shape as buildFinestLevel in segy_pyramid.cpp.
    pool.parallelFor(size_t(traceCount), 1, [&](size_t begin, size_t end) {
        for (size_t t = begin; t < end; ++t) {
            const uint8_t* headerBase = fileBase + kHeaderTotalSize + t * traceStrideBytes;
            TraceHeader h = parseTraceHeader(headerBase);
            columns.traceSequenceLine[t] = h.traceSequenceLine;
            columns.traceSequenceFile[t] = h.traceSequenceFile;
            columns.fieldRecord[t] = h.fieldRecord;
            columns.traceNumber[t] = h.traceNumber;
            columns.cdp[t] = h.cdp;
            columns.x[t] = h.x;
            columns.y[t] = h.y;
        }
    });

    return columns;
}

std::vector<int64_t> selectTraces(const TraceHeaderColumns& columns, const std::vector<IdentCriterion>& criteria) {
    int64_t n = columns.traceCount();
    std::vector<int64_t> result;
    result.reserve(size_t(n));

    if (criteria.empty()) {
        result.resize(size_t(n));
        for (int64_t t = 0; t < n; ++t) result[size_t(t)] = t;
        return result;
    }

    for (int64_t t = 0; t < n; ++t) {
        bool matches = true;
        for (const IdentCriterion& c : criteria) {
            int32_t v = columns.column(c.field)[size_t(t)];
            if (v < c.min || v > c.max) {
                matches = false;
                break;
            }
            if (c.inc > 1 && (v - c.min) % c.inc != 0) {
                matches = false;
                break;
            }
        }
        if (matches) result.push_back(t);
    }

    return result;
}

void sortTraceIndices(std::vector<int64_t>& indices, const TraceHeaderColumns& columns,
                       const std::vector<IdentField>& sortKeys) {
    if (sortKeys.empty()) return;

    // Resolve each key's column once, up front, rather than re-dispatching
    // TraceHeaderColumns::column()'s switch on every comparison.
    std::vector<const std::vector<int32_t>*> keyColumns;
    keyColumns.reserve(sortKeys.size());
    for (IdentField field : sortKeys) keyColumns.push_back(&columns.column(field));

    std::stable_sort(indices.begin(), indices.end(), [&](int64_t a, int64_t b) {
        for (const std::vector<int32_t>* col : keyColumns) {
            int32_t va = (*col)[size_t(a)];
            int32_t vb = (*col)[size_t(b)];
            if (va != vb) return va < vb;
        }
        return false;
    });
}

} // namespace segy
