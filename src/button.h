#pragma once

enum class WakePress {
  CLICK,
  SECONDARY,
  LONG,
  LONGEST,
};

void enterFlashMode();
bool handleKeyButtonAtStartup();
void checkRuntimeButtons();
void checkRuntimeKeyButton();
WakePress detectKeyButtonPress(unsigned long pressStart = 0);
