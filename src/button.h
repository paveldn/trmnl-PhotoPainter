#pragma once

enum class WakePress {
  CLICK,
  MEDIUM,
  LONG,
  LONGEST,
};

bool handleBootButtonReset();
void checkRuntimeReset();
WakePress detectButtonWakePress();

