#pragma once

#include <Arduino.h>

#ifndef APP_ENABLE_SERIAL_LOGS
#define APP_ENABLE_SERIAL_LOGS 0
#endif

#if APP_ENABLE_SERIAL_LOGS
#define APP_SERIAL_BEGIN(baud) do { Serial.begin(baud); } while (false)
#define APP_LOGF(...) do { Serial.printf(__VA_ARGS__); } while (false)
#define APP_LOGLN(...) do { Serial.println(__VA_ARGS__); } while (false)
#define APP_LOG_UPDATE_ERROR(update) do { (update).printError(Serial); } while (false)
#else
// Do not evaluate logging arguments when serial diagnostics are disabled.
#define APP_SERIAL_BEGIN(...) do { } while (false)
#define APP_LOGF(...) do { } while (false)
#define APP_LOGLN(...) do { } while (false)
#define APP_LOG_UPDATE_ERROR(...) do { } while (false)
#endif
