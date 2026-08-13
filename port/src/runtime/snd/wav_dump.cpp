#include "runtime/snd/wav_dump.h"
#include <cstring>

namespace tj::snd {

bool WavDump::Open(const char* path, uint32_t rate, uint32_t channels) {
    Close();
    if (fopen_s(&f_, path, "wb") != 0) { f_ = nullptr; return false; }
    uint8_t h[44] = {};
    memcpy(h, "RIFF", 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 36, "data", 4);
    *(uint32_t*)(h + 16) = 16;                    // fmt chunk size
    *(uint16_t*)(h + 20) = 1;                     // WAVE_FORMAT_PCM
    *(uint16_t*)(h + 22) = (uint16_t)channels;
    *(uint32_t*)(h + 24) = rate;
    *(uint32_t*)(h + 28) = rate * channels * 2;
    *(uint16_t*)(h + 32) = (uint16_t)(channels * 2);
    *(uint16_t*)(h + 34) = 16;
    fwrite(h, 1, sizeof h, f_);
    bytes_ = 0;
    return true;
}

void WavDump::Append(const int16_t* s, size_t count) {
    if (!f_ || !count) return;
    fwrite(s, 2, count, f_);
    bytes_ += count * 2;
    uint32_t riff = (uint32_t)(36 + bytes_), data = (uint32_t)bytes_;
    fseek(f_, 4, SEEK_SET);  fwrite(&riff, 4, 1, f_);
    fseek(f_, 40, SEEK_SET); fwrite(&data, 4, 1, f_);
    fseek(f_, 0, SEEK_END);
    fflush(f_);
}

void WavDump::Close() {
    if (f_) { fclose(f_); f_ = nullptr; }
    bytes_ = 0;
}

} // namespace tj::snd
