#pragma once

#include <M5GFX.h>
#include <M5Unified.h>

#include "IntercomTypes.h"

namespace intercom {

// Compact status display. Uses a sprite when possible and falls back to
// drawing directly on the panel if the allocation fails.
class IntercomUi {
 public:
  bool begin();
  void render(const UiState& state);

 private:
  void drawHeader(lgfx::LGFXBase& g, const UiState& s);
  void drawState(lgfx::LGFXBase& g, const UiState& s);
  void drawPeer(lgfx::LGFXBase& g, const UiState& s);
  void drawBars(lgfx::LGFXBase& g, const UiState& s);
  void drawInfo(lgfx::LGFXBase& g, const UiState& s);
  void drawFooter(lgfx::LGFXBase& g, const UiState& s);

  M5Canvas _canvas{&M5.Display};
  bool _useCanvas = false;
  int _w = 0;
  int _h = 0;
  uint32_t _lastPeerLabelSwitchMs = 0;
  bool _showPeerAddress = false;
  uint32_t _lastHeaderPhaseMs = 0;
  uint32_t _lastHeaderScrollMs = 0;
  uint8_t _headerPhase = 0;
  size_t _headerScrollOffset = 0;
};

}  // namespace intercom
