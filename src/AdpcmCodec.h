#pragma once

#include <stddef.h>
#include <stdint.h>

namespace intercom {

// IMA ADPCM (4 bits per sample, 2 samples per byte).
//
// One encoder instance is used by the transmit task and one decoder instance
// by the receive task. The predictor and step index are carried in every
// packet so a lost packet only damages its own frame.
class AdpcmCodec {
 public:
  void reset();

  // Returns the number of bytes written. `count` should be even.
  size_t encode(const int16_t* samples, size_t count, uint8_t* out);

  // Returns the number of samples written.
  size_t decode(const uint8_t* in, size_t bytes, int16_t* out, size_t maxSamples);

  int16_t predictor() const { return _predictor; }
  uint8_t index() const { return _index; }
  void setState(int16_t predictor, uint8_t index);

 private:
  int16_t _predictor = 0;
  uint8_t _index = 0;
};

}  // namespace intercom
