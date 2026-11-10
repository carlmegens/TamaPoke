#include "batcurve.h"
#include "rtcbat.h"
#include "pin_config.h"  // define XPOWERS_CHIP_AXP2101
#include <Wire.h>
#include <time.h>
#include <SensorPCF85063.hpp>
#include <XPowersLib.h>

static SensorPCF85063 rtc;
static XPowersPMU pmu;
static bool rtcOk = false;
static bool pmuOk = false;

bool rtcBegin() {
  rtcOk = rtc.begin(Wire, IIC_SDA, IIC_SCL);
  if (!rtcOk) Serial.println("PCF85063 no detectado");
  return rtcOk;
}

uint32_t rtcEpoch() {
  if (!rtcOk) return 0;
  RTC_DateTime t = rtc.getDateTime();
  if (t.getYear() < 2025 || t.getYear() > 2120) return 0;  // sin hora valida
  struct tm tmv = {};
  tmv.tm_year = t.getYear() - 1900;
  tmv.tm_mon = t.getMonth() - 1;
  tmv.tm_mday = t.getDay();
  tmv.tm_hour = t.getHour();
  tmv.tm_min = t.getMinute();
  tmv.tm_sec = t.getSecond();
  time_t e = mktime(&tmv);  // TZ por defecto = UTC, consistente con gmtime_r
  return e > 0 ? (uint32_t)e : 0;
}

void rtcSetEpoch(uint32_t e) {
  if (!rtcOk) return;
  time_t tt = e;
  struct tm tmv;
  gmtime_r(&tt, &tmv);
  rtc.setDateTime(RTC_DateTime(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                               tmv.tm_hour, tmv.tm_min, tmv.tm_sec));
}

bool batBegin() {
  pmuOk = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!pmuOk) { Serial.println("AXP2101 no detectado"); return false; }
  pmu.enableBattVoltageMeasure();
  pmu.setSysPowerDownVoltage(BAT_HW_CUTOFF_MV);   // the backstop if the firmware is too late
  return true;
}

// Enciende la alimentacion de la AMOLED. En la Waveshare 1.75 el panel (OLED VDD)
// cuelga del rail BLDO1 a 3.3V del AXP2101. El firmware daba por hecho que estaba
// encendido; si el PMU se resetea (drenaje total), BLDO1 queda OFF y la pantalla
// se ve negra aunque el resto funcione. Hay que llamarla ANTES de gfx->begin().
void pmuEnablePanel() {
  if (!pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    Serial.println("AXP2101 no detectado (pmuEnablePanel)");
    return;
  }
  pmu.setBLDO1Voltage(3300);   // OLED VDD
  pmu.enableBLDO1();
}

// el estado de energia (I2C) se cachea ~2 s: leerlo en cada frame del loop
// metia trafico I2C inutil y podia oscilar (parpadeo de brillo)
static uint32_t powerCacheT = 0;
static int cachedPct = -1;
static bool cachedCharging = false, cachedUsb = true;

static uint32_t pollMs = 2000;
void batSetPollMs(uint32_t ms) { pollMs = ms; }

static BatMonitor mon;
static bool monDischarging = false;

static void refreshPower() {
  uint32_t now = millis();
  if (powerCacheT && now - powerCacheT < pollMs) return;
  powerCacheT = now ? now : 1;
  if (!pmuOk) { cachedPct = -1; cachedCharging = false; cachedUsb = true; return; }
  bool conn = pmu.isBatteryConnect();
  cachedCharging = pmu.isCharging();
  cachedUsb = pmu.isVbusIn();
  // Discharging, the percent comes from a LiPo curve (batcurve.h): the PMU's
  // own gauge reads high right after unplugging. On USB or charging the
  // voltage is lifted by the charger and means nothing, so the PMU's is used.
  bool discharging = conn && !cachedUsb && !cachedCharging;
  if (discharging != monDischarging) {
    mon.reset();
    mon.seed(cachedPct);   // unplugging never RAISES the percent
    monDischarging = discharging;
  }
  if (discharging) cachedPct = mon.sample((int)pmu.getBattVoltage(), now);
  else cachedPct = conn ? pmu.getBatteryPercent() : -1;
  if (cachedPct < 0 && conn) cachedPct = pmu.getBatteryPercent();   // no voltage reading
}

int batPercent() { refreshPower(); return cachedPct; }
bool batCharging() { refreshPower(); return cachedCharging; }
bool usbPresent() { refreshPower(); return cachedUsb; }
bool usbPresentNow() { return pmuOk ? pmu.isVbusIn() : true; }
bool batShouldShutdown() { refreshPower(); return monDischarging && mon.shutdownNeeded(millis()); }
void batPowerOff() {
  if (pmuOk) pmu.shutdown();
  mon.reset();   // still alive (USB plugged in that instant): the whole wait starts again
}

void pwrSetup() {
  if (!pmuOk) return;
  pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_4S);   // the hardware backstop: its shortest
  pmu.setIrqLevelTime(XPOWERS_AXP2101_IRQ_TIME_2S);    // what counts as a long press
  pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ);
  pmu.clearIrqStatus();
}

// One read of the PMU's latched status, both kinds answered from it, then
// cleared -- reading it twice could clear one kind while asking about the other.
uint8_t pwrPoll() {
  if (!pmuOk) return PWR_NONE;
  pmu.getIrqStatus();
  bool shortHit = pmu.isPekeyShortPressIrq();
  bool longHit = pmu.isPekeyLongPressIrq();
  if (shortHit || longHit) pmu.clearIrqStatus();
  return longHit ? PWR_LONG : (shortHit ? PWR_SHORT : PWR_NONE);
}

// Serial PWR. A battery the PMU does not see shows as "no battery" everywhere
// else; this prints what the chip itself reports, with the raw battery ADC read
// even when the present bit is clear.
void batDebug() {
  if (!pmuOk) { Serial.println("PWR pmu=missing"); return; }
  int st1 = pmu.readRegister(0x00), st2 = pmu.readRegister(0x01), det = pmu.readRegister(0x68);
  int hi = pmu.readRegister(0x34), lo = pmu.readRegister(0x35);
  int rawMv = (hi >= 0 && lo >= 0) ? (((hi & 0x1F) << 8) | lo) : -1;
  Serial.printf("PWR present=%d active=%d vbus=%d charging=%d chg_state=%d st1=0x%02X st2=0x%02X detctrl=0x%02X\n",
                pmu.isBatteryConnect(), (st1 >> 2) & 1, pmu.isVbusIn(), pmu.isCharging(), st2 & 0x07, st1, st2, det);
  Serial.printf("PWR vbat_raw=%dmV vbus=%umV vsys=%umV pct=%d chg_curr_code=%u target_code=%u\n", rawMv,
                pmu.getVbusVoltage(), pmu.getSystemVoltage(), pmu.getBatteryPercent(),
                pmu.getChargerConstantCurr(), pmu.getChargeTargetVoltage());
}
