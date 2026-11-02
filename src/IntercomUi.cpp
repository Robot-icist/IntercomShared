#include "IntercomUi.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "IntercomConfig.h"

namespace intercom {
namespace {

uint16_t stateColor(State state) {
  switch (state) {
    case State::Tx:
      return TFT_RED;
    case State::Rec:
      return TFT_ORANGE;
    case State::Send:
      return TFT_CYAN;
    case State::Rx:
      return TFT_SKYBLUE;
    case State::Error:
      return TFT_RED;
    default:
      return TFT_GREEN;
  }
}

int clampBar(float level, int width) {
  int value = (int)(level * (float)width);
  return std::max(0, std::min(width, value));
}

}  // namespace

bool IntercomUi::begin() {
  M5.Display.setRotation(0);
  M5.Display.setBrightness(200);
  M5.Display.fillScreen(TFT_BLACK);

  _w = M5.Display.width();
  _h = M5.Display.height();
  if (_w <= 0 || _h <= 0) {
    return false;
  }

  _canvas.setColorDepth(16);
  _canvas.setPsram(kUiUsePsram);
  _useCanvas = _canvas.createSprite(_w, _h) != nullptr;
  if (!_useCanvas) {
    Serial.println("[ui] sprite allocation failed, drawing directly");
  }
  return true;
}

void IntercomUi::render(const UiState& state) {
  if (_w <= 0 || _h <= 0) {
    return;
  }

  lgfx::LGFXBase* g = _useCanvas ? (lgfx::LGFXBase*)&_canvas
                                 : (lgfx::LGFXBase*)&M5.Display;
  g->startWrite();
  g->fillScreen(TFT_BLACK);

  drawHeader(*g, state);
  drawState(*g, state);
  drawPeer(*g, state);
  drawBars(*g, state);
  drawInfo(*g, state);
  drawFooter(*g, state);

  g->endWrite();
  if (_useCanvas) {
    _canvas.pushSprite(0, 0);
  }
}

void IntercomUi::drawHeader(lgfx::LGFXBase& g, const UiState& s) {
  g.setTextSize(1);
  g.setTextDatum(lgfx::textdatum_t::top_left);
  g.setTextColor(TFT_LIGHTGREY);
  g.drawString(INTERCOM_DEVICE_NAME, 4, 2);

  char battery[12];
  if (s.battery < 0) {
    snprintf(battery, sizeof(battery), "USB");
  } else {
    snprintf(battery, sizeof(battery), "%d%%", s.battery);
  }
  g.setTextDatum(lgfx::textdatum_t::top_right);
  g.setTextColor(s.battery >= 0 && s.battery < 20 ? TFT_RED : TFT_LIGHTGREY);
  g.drawString(battery, _w - 4, 2);

  g.drawFastHLine(0, 12, _w, TFT_DARKGREY);
}

void IntercomUi::drawState(lgfx::LGFXBase& g, const UiState& s) {
  g.setTextDatum(lgfx::textdatum_t::top_left);
  g.setTextSize(2);
  if (s.mode == Mode::Volume) {
    g.setTextColor(TFT_YELLOW);
    g.drawString("VOLUME", 4, 18);
  } else {
    g.setTextColor(stateColor(s.state));
    g.drawString(stateName(s.state), 4, 18);
  }

  g.setTextDatum(lgfx::textdatum_t::top_right);
  g.setTextSize(1);
  g.setTextColor(TFT_LIGHTGREY);
  g.drawString(modeName(s.mode), _w - 4, 22);
}

void IntercomUi::drawPeer(lgfx::LGFXBase& g, const UiState& s) {
  const int y = 40;
  g.fillCircle(7, y + 4, 3, s.peerOnline ? TFT_GREEN : TFT_DARKGREY);

  char name[9] = {0};
  if (s.peerOnline && s.peerName[0]) {
    strncpy(name, s.peerName, sizeof(name) - 1);
  } else {
    strncpy(name, "NO PEER", sizeof(name) - 1);
  }

  g.setTextSize(1);
  g.setTextDatum(lgfx::textdatum_t::top_left);
  g.setTextColor(s.peerOnline ? TFT_WHITE : TFT_DARKGREY);
  g.drawString(name, 14, y);

  if (s.peerOnline) {
    char rssi[12];
    snprintf(rssi, sizeof(rssi), "%d dBm", (int)s.peerRssi);
    g.setTextDatum(lgfx::textdatum_t::top_right);
    g.setTextColor(TFT_LIGHTGREY);
    g.drawString(rssi, _w - 4, y);
  }
}

void IntercomUi::drawBars(lgfx::LGFXBase& g, const UiState& s) {
  const int x = 34;
  const int w = _w - x - 4;
  const int h = 8;
  const int y1 = 54;
  const int y2 = 68;

  g.setTextSize(1);
  g.setTextDatum(lgfx::textdatum_t::top_left);
  g.setTextColor(TFT_LIGHTGREY);
  g.drawString("MIC", 4, y1);
  g.drawString("RX", 4, y2);

  g.drawRect(x - 1, y1 - 1, w + 2, h + 2, TFT_DARKGREY);
  g.drawRect(x - 1, y2 - 1, w + 2, h + 2, TFT_DARKGREY);

  int mic = clampBar(s.micLevel, w);
  int rx = clampBar(s.rxLevel, w);
  if (mic > 0) {
    g.fillRect(x, y1, mic, h, TFT_GREEN);
  }
  if (rx > 0) {
    g.fillRect(x, y2, rx, h, TFT_CYAN);
  }
}

void IntercomUi::drawInfo(lgfx::LGFXBase& g, const UiState& s) {
  const int y = (_h > 150) ? 150 : 84;
  g.setTextSize(1);
  g.setTextDatum(lgfx::textdatum_t::top_left);
  g.setTextColor(TFT_DARKGREY);

  char left[24];
  snprintf(left, sizeof(left), "CH %u  %u kHz", (unsigned)s.channel,
           (unsigned)(s.sampleRate / 1000));
  g.drawString(left, 4, y);

  g.setTextDatum(lgfx::textdatum_t::top_right);
  if (s.mode == Mode::Volume) {
    char volume[12];
    snprintf(volume, sizeof(volume), "VOL %u%%", (unsigned)s.volume);
    g.drawString(volume, _w - 4, y);
  } else {
    g.drawString("ESP-NOW", _w - 4, y);
  }
}

void IntercomUi::drawFooter(lgfx::LGFXBase& g, const UiState& s) {
  const int y = _h - 26;
  g.drawFastHLine(0, y - 4, _w, TFT_DARKGREY);

  char line1[24];
  char line2[24];
  if (s.mode == Mode::Volume) {
    snprintf(line1, sizeof(line1), "VOL %u%%", (unsigned)s.volume);
#if defined(INTERCOM_BOARD_STICKS3)
    snprintf(line2, sizeof(line2), "A+ B- 2X NEXT");
#else
    snprintf(line2, sizeof(line2), "CLK+ HLD- 2X");
#endif
  } else if (s.mode == Mode::Live) {
    snprintf(line1, sizeof(line1), "HOLD A = TALK");
    snprintf(line2, sizeof(line2), "2X A = MODE");
  } else if (s.state == State::Rec) {
    snprintf(line1, sizeof(line1), "CLICK A = STOP");
    snprintf(line2, sizeof(line2), "2X A = CANCEL");
  } else if (s.state == State::Send) {
    snprintf(line1, sizeof(line1), "SENDING %u%%", (unsigned)s.sendProgress);
    snprintf(line2, sizeof(line2), "PLEASE WAIT");
  } else {
    snprintf(line1, sizeof(line1), "CLICK A = REC");
    snprintf(line2, sizeof(line2), "2X A = MODE");
  }

  g.setTextSize(1);
  g.setTextDatum(lgfx::textdatum_t::top_left);
  g.setTextColor(TFT_DARKGREY);
  g.drawString(line1, 4, y);
  g.drawString(line2, 4, y + 10);

  if (s.state == State::Rec) {
    char time[12];
    snprintf(time, sizeof(time), "%u.%us", (unsigned)(s.recordingMs / 1000),
             (unsigned)((s.recordingMs % 1000) / 100));
    g.setTextDatum(lgfx::textdatum_t::top_right);
    g.setTextColor(TFT_ORANGE);
    g.drawString(time, _w - 4, y);
  }

  if (_h > 150) {
    const int boxY = 104;
    g.drawRoundRect(6, boxY, _w - 12, 36, 8, TFT_DARKGREY);
    g.setTextDatum(lgfx::textdatum_t::middle_center);
    g.setTextSize(2);
    g.setTextColor(TFT_WHITE);
    char mode[20];
    snprintf(mode, sizeof(mode), "%s MODE", modeName(s.mode));
    g.drawString(mode, _w / 2, boxY + 18);
  }
}

}  // namespace intercom
