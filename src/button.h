#pragma once

enum class WakePress {
  CLICK,
  LONG,
  LONGEST,
};

void enterFlashMode();
bool handleKeyButtonAtStartup();
void checkRuntimeButtons();
WakePress detectKeyButtonPress();
