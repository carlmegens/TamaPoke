#pragma once
#include <Arduino.h>

// RTC PCF85063: hora persistente mientras la placa tenga alimentacion
bool rtcBegin();
uint32_t rtcEpoch();             // segundos unix; 0 si el RTC no es valido
void rtcSetEpoch(uint32_t e);

// PMU AXP2101: estado de la bateria
bool batBegin();
void pmuEnablePanel();           // enciende BLDO1 (OLED VDD 3.3V); llamar antes de gfx->begin()
int batPercent();                // 0-100, -1 si no hay bateria conectada
void batDebug();                 // serial PWR: the PMU registers that say why a battery is or is not seen
bool batCharging();
bool usbPresent();
bool usbPresentNow();            // uncached (one I2C read): for deciding whether to light-sleep
void batSetPollMs(uint32_t ms);  // how often the PMU is read (longer with the screen off)
bool batShouldShutdown();        // drained to the safe limit (batcurve.h): save and power off
void batPowerOff();              // the PMU cuts the power; the RTC keeps the time

// boton PWR del AXP2101: pulsacion larga 4s = apagado fisico (RTC sigue vivo);
// la pulsacion corta la captura el firmware (pantalla on/off)
void pwrSetup();
enum : uint8_t { PWR_NONE = 0, PWR_SHORT, PWR_LONG };
uint8_t pwrPoll();       // poll in the loop: a short press, a 2 s hold, or nothing
