#include "segy_writer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <future>

namespace segy {

namespace {

std::FILE* openForWrite(const std::filesystem::path& path) {
#if defined(_WIN32)
    return _wfopen(path.wstring().c_str(), L"wb");
#else
    return std::fopen(path.string().c_str(), "wb");
#endif
}

bool writeFileHeaders(std::FILE* f, const std::string& textHeaderAscii, BinaryHeader binHeader) {
    binHeader.formatCode = kIeeeFloat32;
    std::string ebcdic = encodeEbcdicText(textHeaderAscii);
    bool ok = std::fwrite(ebcdic.data(), 1, ebcdic.size(), f) == ebcdic.size();
    uint8_t binBuf[kBinaryHeaderSize];
    writeBinaryHeader(binBuf, binHeader);
    return ok && std::fwrite(binBuf, 1, sizeof(binBuf), f) == sizeof(binBuf);
}

// One buffer per call: header then big-endian IEEE samples, one fwrite.
bool writeTraces(std::FILE* f, const WriteTrace* traces, size_t count, std::vector<uint8_t>& buffer) {
    for (size_t k = 0; k < count; ++k) {
        const WriteTrace& tr = traces[k];
        buffer.resize(tr.headerBytes.size() + tr.samples.size() * 4);
        std::memcpy(buffer.data(), tr.headerBytes.data(), tr.headerBytes.size());
        uint8_t* out = buffer.data() + tr.headerBytes.size();
        for (size_t i = 0; i < tr.samples.size(); ++i) {
            uint32_t bits;
            std::memcpy(&bits, &tr.samples[i], sizeof(bits));
            writeU32BE(out + i * 4, bits);
        }
        if (std::fwrite(buffer.data(), 1, buffer.size(), f) != buffer.size()) return false;
    }
    return true;
}

bool finish(std::FILE* f, bool ok, const std::filesystem::path& path, std::string* error) {
    const bool closeOk = std::fclose(f) == 0;
    if (!ok || !closeOk) {
        if (error) *error = "Failed writing file: " + path.string();
        return false;
    }
    return true;
}

} // namespace

bool writeSegyFile(const std::filesystem::path& path, const std::string& textHeaderAscii, BinaryHeader binHeader,
                   const std::vector<WriteTrace>& traces, std::string* error) {
    std::FILE* f = openForWrite(path);
    if (!f) {
        if (error) *error = "Could not create file: " + path.string();
        return false;
    }
    std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
    std::vector<uint8_t> buffer;
    const bool ok = writeFileHeaders(f, textHeaderAscii, binHeader) && writeTraces(f, traces.data(), traces.size(), buffer);
    return finish(f, ok, path, error);
}

bool writeSegyFileChunked(const std::filesystem::path& path, const std::string& textHeaderAscii, BinaryHeader binHeader,
                          int64_t traceCount, const TraceChunkFill& fill, std::string* error, int64_t chunkTraces) {
    std::FILE* f = openForWrite(path);
    if (!f) {
        if (error) *error = "Could not create file: " + path.string();
        return false;
    }
    std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
    bool ok = writeFileHeaders(f, textHeaderAscii, binHeader);
    chunkTraces = std::max<int64_t>(1, chunkTraces);
    // Two chunks: one filled here while the other is written by `pending`.
    std::vector<WriteTrace> chunks[2];
    std::vector<uint8_t> buffer;
    std::future<bool> pending;
    int current = 0;
    for (int64_t begin = 0; ok && begin < traceCount; begin += chunkTraces) {
        const int64_t end = std::min(traceCount, begin + chunkTraces);
        std::vector<WriteTrace>& chunk = chunks[current];
        chunk.resize(size_t(end - begin));
        fill(begin, end, chunk);
        if (pending.valid()) ok = pending.get();
        if (!ok) break;
        pending = std::async(std::launch::async, [f, &chunk, &buffer]() {
            return writeTraces(f, chunk.data(), chunk.size(), buffer);
        });
        current = 1 - current;
    }
    if (pending.valid()) ok = pending.get() && ok;
    return finish(f, ok, path, error);
}

} // namespace segy
