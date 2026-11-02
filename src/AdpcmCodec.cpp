#include "AdpcmCodec.h"

#include <algorithm>

namespace intercom {
namespace {

constexpr int8_t kIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

constexpr int16_t kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,
    21,    23,    25,    28,    31,    34,    37,    41,    45,    50,    55,
    60,    66,    73,    80,    88,    97,    107,   118,   130,   143,   157,
    173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,
    494,   544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,
    1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,  3660,
    4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
};

int16_t clamp16(int32_t value) {
  if (value > 32767) {
    return 32767;
  }
  if (value < -32768) {
    return -32768;
  }
  return (int16_t)value;
}

}  // namespace

void AdpcmCodec::reset() {
  _predictor = 0;
  _index = 0;
}

void AdpcmCodec::setState(int16_t predictor, uint8_t index) {
  _predictor = predictor;
  _index = std::min<uint8_t>(index, 88);
}

size_t AdpcmCodec::encode(const int16_t* samples, size_t count, uint8_t* out) {
  size_t bytes = 0;
  for (size_t i = 0; i + 1 < count; i += 2) {
    uint8_t packed = 0;
    for (int n = 0; n < 2; ++n) {
      int32_t sample = samples[i + n];
      int32_t step = kStepTable[_index];
      int32_t diff = sample - _predictor;
      uint8_t nibble = 0;

      if (diff < 0) {
        nibble = 8;
        diff = -diff;
      }

      int32_t vpdiff = step >> 3;
      if (diff >= step) {
        nibble |= 4;
        diff -= step;
        vpdiff += step;
      }
      step >>= 1;
      if (diff >= step) {
        nibble |= 2;
        diff -= step;
        vpdiff += step;
      }
      step >>= 1;
      if (diff >= step) {
        nibble |= 1;
        vpdiff += step;
      }

      _predictor = clamp16(_predictor + ((nibble & 8) ? -vpdiff : vpdiff));
      _index = (uint8_t)std::min<int>(88, std::max<int>(0, _index + kIndexTable[nibble]));
      packed |= (uint8_t)((nibble & 0x0F) << (n == 0 ? 4 : 0));
    }
    out[bytes++] = packed;
  }
  return bytes;
}

size_t AdpcmCodec::decode(const uint8_t* in, size_t bytes, int16_t* out,
                          size_t maxSamples) {
  size_t samples = 0;
  for (size_t i = 0; i < bytes && samples + 2 <= maxSamples; ++i) {
    for (int n = 0; n < 2; ++n) {
      uint8_t nibble = (in[i] >> (n == 0 ? 4 : 0)) & 0x0F;
      int32_t step = kStepTable[_index];
      int32_t vpdiff = step >> 3;

      if (nibble & 4) {
        vpdiff += step;
      }
      step >>= 1;
      if (nibble & 2) {
        vpdiff += step;
      }
      step >>= 1;
      if (nibble & 1) {
        vpdiff += step;
      }

      _predictor = clamp16(_predictor + ((nibble & 8) ? -vpdiff : vpdiff));
      _index = (uint8_t)std::min<int>(88, std::max<int>(0, _index + kIndexTable[nibble]));
      out[samples++] = _predictor;
    }
  }
  return samples;
}

}  // namespace intercom
