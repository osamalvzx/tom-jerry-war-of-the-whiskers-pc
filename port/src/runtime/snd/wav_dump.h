// Tiny RIFF/WAVE PCM16 writer for the TJ_SND_DUMP diagnostic (dsound_bridge):
// dumps the exact int16 samples handed to XAudio2 so decode quality can be
// analyzed offline. Header sizes are re-patched on every Append, so a killed
// process (the normal end of a hybrid test run) still leaves a valid file.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>

namespace tj::snd {

class WavDump {
public:
    ~WavDump() { Close(); }
    bool Open(const char* path, uint32_t rate, uint32_t channels);
    void Append(const int16_t* s, size_t count);   // count = int16 values
    void Close();
    bool   IsOpen() const { return f_ != nullptr; }
    size_t DataBytes() const { return bytes_; }
private:
    FILE*  f_ = nullptr;
    size_t bytes_ = 0;
};

} // namespace tj::snd
