#pragma once
// IMA ADPCM (4 bits por muestra). Cada paquete lleva su estado inicial, así que
// los paquetes se decodifican de forma independiente (tolerante a pérdidas).
#include <cstdint>

namespace adpcm {

static const int kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static const int kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

struct State {
    int16_t predictor = 0;
    uint8_t index = 0;
};

inline uint8_t EncodeSample(State& s, int16_t sample) {
    int step = kStepTable[s.index];
    int diff = sample - s.predictor;
    uint8_t code = 0;
    if (diff < 0) { code = 8; diff = -diff; }
    int delta = step >> 3;
    if (diff >= step) { code |= 4; diff -= step; delta += step; }
    step >>= 1;
    if (diff >= step) { code |= 2; diff -= step; delta += step; }
    step >>= 1;
    if (diff >= step) { code |= 1; delta += step; }
    int pred = s.predictor + ((code & 8) ? -delta : delta);
    if (pred > 32767) pred = 32767;
    if (pred < -32768) pred = -32768;
    s.predictor = (int16_t)pred;
    int idx = s.index + kIndexTable[code];
    s.index = (uint8_t)(idx < 0 ? 0 : idx > 88 ? 88 : idx);
    return code;
}

inline int16_t DecodeSample(State& s, uint8_t code) {
    int step = kStepTable[s.index];
    int delta = step >> 3;
    if (code & 4) delta += step;
    if (code & 2) delta += step >> 1;
    if (code & 1) delta += step >> 2;
    int pred = s.predictor + ((code & 8) ? -delta : delta);
    if (pred > 32767) pred = 32767;
    if (pred < -32768) pred = -32768;
    s.predictor = (int16_t)pred;
    int idx = s.index + kIndexTable[code];
    s.index = (uint8_t)(idx < 0 ? 0 : idx > 88 ? 88 : idx);
    return s.predictor;
}

// n muestras -> n/2 bytes
inline void Encode(State& s, const int16_t* in, int n, uint8_t* out) {
    for (int i = 0; i < n; i += 2) {
        uint8_t lo = EncodeSample(s, in[i]);
        uint8_t hi = EncodeSample(s, in[i + 1]);
        out[i / 2] = (uint8_t)(lo | (hi << 4));
    }
}

inline void Decode(State& s, const uint8_t* in, int n, int16_t* out) {
    for (int i = 0; i < n; i += 2) {
        out[i] = DecodeSample(s, in[i / 2] & 0x0F);
        out[i + 1] = DecodeSample(s, in[i / 2] >> 4);
    }
}

}  // namespace adpcm
