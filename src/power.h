#pragma once

float getBatteryVoltage();
float readBatteryAvg(int samples, int delayMs);
bool isExternalPowerPresent();
bool isBatteryCharging();
void showLowBatteryAndShutdown();
void goToDeepSleep(int seconds);
void initPower();

