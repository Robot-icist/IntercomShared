#include "SampleRing.h"

#include <algorithm>
#include <cstring>

#include <esp_heap_caps.h>

namespace intercom {

SampleRing::~SampleRing() {
  end();
}

bool SampleRing::begin(size_t capacity) {
  end();
  if (capacity == 0) {
    return false;
  }

  _buffer = (int16_t*)heap_caps_malloc(capacity * sizeof(int16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!_buffer) {
    _buffer = (int16_t*)malloc(capacity * sizeof(int16_t));
  }
  if (!_buffer) {
    return false;
  }

  _mutex = xSemaphoreCreateMutex();
  if (!_mutex) {
    free(_buffer);
    _buffer = nullptr;
    return false;
  }

  _capacity = capacity;
  _head = 0;
  _tail = 0;
  _size = 0;
  return true;
}

void SampleRing::end() {
  if (_mutex) {
    vSemaphoreDelete(_mutex);
    _mutex = nullptr;
  }
  if (_buffer) {
    free(_buffer);
    _buffer = nullptr;
  }
  _capacity = 0;
  _head = 0;
  _tail = 0;
  _size = 0;
}

size_t SampleRing::write(const int16_t* data, size_t count) {
  if (!_mutex || !_buffer || !data || count == 0) {
    return 0;
  }

  xSemaphoreTake(_mutex, portMAX_DELAY);

  // Keep the newest audio if the caller tries to write more than the ring can
  // hold at once.
  if (count > _capacity) {
    data += count - _capacity;
    count = _capacity;
    _head = 0;
    _tail = 0;
    _size = 0;
  }

  size_t overflow = (_size + count > _capacity) ? (_size + count - _capacity) : 0;
  if (overflow) {
    _tail = (_tail + overflow) % _capacity;
    _size -= overflow;
  }

  size_t first = std::min(count, _capacity - _head);
  memcpy(_buffer + _head, data, first * sizeof(int16_t));
  if (count > first) {
    memcpy(_buffer, data + first, (count - first) * sizeof(int16_t));
  }
  _head = (_head + count) % _capacity;
  _size += count;

  xSemaphoreGive(_mutex);
  return count;
}

size_t SampleRing::read(int16_t* data, size_t count) {
  if (!_mutex || !_buffer || !data || count == 0) {
    return 0;
  }

  xSemaphoreTake(_mutex, portMAX_DELAY);

  size_t n = std::min(count, _size);
  size_t first = std::min(n, _capacity - _tail);
  memcpy(data, _buffer + _tail, first * sizeof(int16_t));
  if (n > first) {
    memcpy(data + first, _buffer, (n - first) * sizeof(int16_t));
  }
  _tail = (_tail + n) % _capacity;
  _size -= n;

  xSemaphoreGive(_mutex);
  return n;
}

size_t SampleRing::available() {
  if (!_mutex) {
    return 0;
  }
  xSemaphoreTake(_mutex, portMAX_DELAY);
  size_t n = _size;
  xSemaphoreGive(_mutex);
  return n;
}

void SampleRing::clear() {
  if (!_mutex) {
    return;
  }
  xSemaphoreTake(_mutex, portMAX_DELAY);
  _head = 0;
  _tail = 0;
  _size = 0;
  xSemaphoreGive(_mutex);
}

}  // namespace intercom
