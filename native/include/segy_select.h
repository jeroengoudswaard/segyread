// Header-based trace selection/sorting: given a file already loaded (mmap'd,
// pyramid built), let the user filter to a subset of traces by trace-header
// field ranges and/or reorder them by header field(s), then rebuild the
// pyramid over exactly that (filtered, reordered) list -- see
// segy_pyramid.h's BuildParams::traceIndexMap, which is what actually lets
// buildPyramid() read an arbitrary trace order without duplicating the
// mmap'd file or re-touching disk (the OS page cache already holds every
// trace touched by the original load).
//
// This is deliberately three small, separately-testable pieces (header
// scan, filter, sort) rather than one "select and sort" function -- the
// UI's Selection dialog can change filter criteria and sort keys
// independently, and re-running just the cheap parts (filter/sort, both
// pure array work over already-scanned columns) instead of rescanning
// headers every time is the difference between an interactive dialog and a
// sluggish one on a large file.
#pragma once
#include <cstdint>
#include <vector>
#include "segy_format.h"
#include "threadpool.h"

namespace segy {

// Which parsed TraceHeader field an ident selection/sort criterion refers
// to -- mirrors TraceHeader's own fields one-for-one (segy_format.h).
enum class IdentField {
    TraceSequenceLine,
    TraceSequenceFile,
    FieldRecord,
    TraceNumber,
    Cdp,
    X,
    Y,
};

// Every trace's header, parsed once and stored column-major (one vector per
// field, indexed by original file trace order) rather than as an array of
// TraceHeader structs -- selectTraces()/sortTraceIndices() each scan one
// field at a time across every trace, and a column-major layout keeps that
// a sequential memory scan instead of striding through a 7-int struct per
// trace for a single field's values.
struct TraceHeaderColumns {
    std::vector<int32_t> traceSequenceLine;
    std::vector<int32_t> traceSequenceFile;
    std::vector<int32_t> fieldRecord;
    std::vector<int32_t> traceNumber;
    std::vector<int32_t> cdp;
    std::vector<int32_t> x;
    std::vector<int32_t> y;

    int64_t traceCount() const { return int64_t(traceSequenceLine.size()); }
    const std::vector<int32_t>& column(IdentField field) const;
};

// Parses every trace's 240-byte header (already resident in the OS page
// cache after the initial pyramid build touched each trace's neighboring
// sample bytes -- this never re-hits disk) into columnar form, in parallel
// across `pool`. O(traceCount), independent of samplesPerTrace -- much
// cheaper than a pyramid rebuild over the same file, since it never decodes
// a single sample.
TraceHeaderColumns scanTraceHeaders(const uint8_t* fileBase, int64_t traceCount, size_t traceStrideBytes,
                                     ThreadPool& pool);

// One Selection-dialog row: keep a trace when `field`'s value falls in
// [min, max] AND, if inc > 1, lands exactly on a min + k*inc step --
// mirrors the reference tool's Min/Max/Inc convention (a stride over the
// ident's own value domain, not over trace position). inc <= 1 means every
// value in [min, max] passes.
struct IdentCriterion {
    IdentField field;
    int32_t min = 0;
    int32_t max = 0;
    int32_t inc = 1;
};

// Original-file trace indices (ascending, file order) satisfying every
// criterion (AND across criteria) -- an empty `criteria` selects every
// trace. Single pass, O(traceCount * criteria.size()); not parallelized
// since it's already cheap relative to the header scan or a pyramid
// rebuild (plain integer comparisons, no memory beyond the columns).
std::vector<int64_t> selectTraces(const TraceHeaderColumns& columns, const std::vector<IdentCriterion>& criteria);

// Stable-sorts `indices` in place by `sortKeys` in priority order (first
// key primary, ties broken by the next, and so on), each ascending --
// mirrors the reference tool's "Sort by" chain of ident fields. No-op for
// an empty `sortKeys`.
void sortTraceIndices(std::vector<int64_t>& indices, const TraceHeaderColumns& columns,
                       const std::vector<IdentField>& sortKeys);

} // namespace segy
