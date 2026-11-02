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
};

}  // namespace intercom
