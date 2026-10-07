#include "segy_writer.h"

#include <cstdio>
#include <cstring>

namespace segy {

bool writeSegyFile(const std::filesystem::path& path, const std::string& textHeaderAscii, BinaryHeader binHeader,
                    const std::vector<WriteTrace>& traces, std::string* error) {
    binHeader.formatCode = kIeeeFloat32;

    std::FILE* f =
#if defined(_WIN32)
        _wfopen(path.wstring().c_str(), L"wb");
#else
        std::fopen(path.string().c_str(), "wb");
#endif
    if (!f) {
        if (error) *error = "Could not create file: " + path.string();
        return false;
    }

    bool ok = true;
    std::string ebcdic = encodeEbcdicText(textHeaderAscii);
    ok = ok && std::fwrite(ebcdic.data(), 1, ebcdic.size(), f) == ebcdic.size();

    uint8_t binBuf[kBinaryHeaderSize];
    writeBinaryHeader(binBuf, binHeader);
    ok = ok && std::fwrite(binBuf, 1, sizeof(binBuf), f) == sizeof(binBuf);

    std::vector<uint8_t> sampleBuf;
    for (const WriteTrace& tr : traces) {
        if (!ok) break;
        ok = ok && std::fwrite(tr.headerBytes.data(), 1, tr.headerBytes.size(), f) == tr.headerBytes.size();

        sampleBuf.resize(tr.samples.size() * 4);
        for (size_t i = 0; i < tr.samples.size(); ++i) {
            uint32_t bits;
            float v = tr.samples[i];
            std::memcpy(&bits, &v, sizeof(bits));
            writeU32BE(sampleBuf.data() + i * 4, bits);
        }
        ok = ok && std::fwrite(sampleBuf.data(), 1, sampleBuf.size(), f) == sampleBuf.size();
    }

    bool closeOk = std::fclose(f) == 0;
    if (!ok || !closeOk) {
        if (error) *error = "Failed writing file: " + path.string();
        return false;
    }
    return true;
}

} // namespace segy
