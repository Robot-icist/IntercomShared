#pragma once

#include <stddef.h>
#include <stdint.h>

#include <Arduino.h>

namespace intercom {

// Thread-safe ring of 16-bit PCM samples.
//
// The producer and consumer are different tasks. When the buffer is full the
// oldest samples are dropped so live audio latency stays bounded.
class SampleRing {
 public:
  SampleRing() = default;
  ~SampleRing();

  SampleRing(const SampleRing&) = delete;
  SampleRing& operator=(const SampleRing&) = delete;

  bool begin(size_t capacity);
  void end();

  size_t write(const int16_t* data, size_t count);
  size_t read(int16_t* data, size_t count);
  size_t available();
  void clear();
  size_t capacity() const { return _capacity; }

 private:
  int16_t* _buffer = nullptr;
  size_t _capacity = 0;
  size_t _head = 0;
  size_t _tail = 0;
  size_t _size = 0;
  SemaphoreHandle_t _mutex = nullptr;
};

}  // namespace intercom
