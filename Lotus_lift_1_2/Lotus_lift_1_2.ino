#include <SPI.h>
#include <Ethernet.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WiFiManager.h>   // Library Manager: search "WiFiManager" by tzapu
#include <Ticker.h>

// ============================================================
//  ESP32 + W5500 — Lotus Side (Lift 1 & 2) + Temperature
//  Ethernet = primary link. WiFi = automatic fallback.
//  Direct HTTP POST -> InfluxDB (no MQTT)
//
//  ---- DATABASE SCHEMA ----
//      10 = ALARM / PRESSED / HIGH TEMP
//       5 = NORMAL
//       0 = OFFLINE / DISCONNECTED / SENSOR FAULT
//
//  Measurements written:
//    node_status,node=<NODE_NAME>  temp=<float>,<LIFT_A>=<code>i,<LIFT_B>=<code>i
//
//  ---- BOOT FLOW ----
//  Ethernet is tried first but with a bounded wait so boot stays
//  fast: quick link check, then a short DHCP timeout. If Ethernet
//  isn't ready within its budget, WiFi is tried immediately
//  (bounded too). If neither connects in time, boot continues
//  anyway and the normal non-blocking retry logic in loop() keeps
//  trying both in the background. Holding the WiFi-setup button
//  at any point during boot interrupts this and opens the config
//  portal immediately.
// ============================================================

// ╔══════════════════════════════════════════════════════════════╗
// ║              *** USER CONFIGURATION SECTION ***              ║
// ║  Change deployment-specific values only in this section.     ║
// ╚══════════════════════════════════════════════════════════════╝
#define STATIC_IP 192, 168, 1, 101
#define INFLUX_HOST "124.43.179.232"
#define INFLUX_PORT 8086
#define INFLUX_ORG "SLT"
#define INFLUX_BUCKET "Lift_Emergency_Alarm"
#define INFLUX_TOKEN "jsEgn9UpR2yTjXaJpSNdwOEow1rnzqMDXPlBuriQaFiq9Mj9X0UIBgu0RN1_kmQMnrCvDojdgM3TYboVSo0D-Q=="
#define NODE_TAG "node1"
#define NODE_NAME "Lotus"
#define LIFT_A_NAME "lift1"
#define LIFT_B_NAME "lift2"
#define AP_NAME "LOTUS-Lift-Node-Wifi-Setting"
#define AP_PASSWORD "SLT_power_operation"

// ╚══════════════════════════════════════════════════════════════╝
//              *** END OF USER CONFIGURATION ***
// ╚══════════════════════════════════════════════════════════════╝

// ---------- W5500 pins ----------
#define W5500_CS 5
#define W5500_RST 4
#define SPI_SCK 18
#define SPI_MISO 19
#define SPI_MOSI 23

// ---------- Ethernet ----------
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0x01 };
IPAddress ip(STATIC_IP);
EthernetClient ethClient;

// ---------- WiFi (fallback only) ----------
WiFiClient wifiClient;

// ---------- InfluxDB ----------
// ---------- Node Identity ----------
const char LIFT_A[] = LIFT_A_NAME;
const char LIFT_B[] = LIFT_B_NAME;

// ---------- Status code constants ----------
const int STATE_ALARM    = 10;
const int STATE_NORMAL   = 5;
const int STATE_OFFLINE  = 0;

// ---------- Temperature thresholds ----------
const float TEMP_HIGH_THRESHOLD_C = 40.0f;

// ---------- Lift buttons ----------
const int liftAPin = 32;
const int liftBPin = 33;

// ---------- LEDs ----------
const int LED_POWER = 26;
const int LED_ETH   = 27;
const int LED_WIFI  = 25;   // optional: lit while running on WiFi

// ---------- Dedicated WiFi setup button ----------
// Separate from the lift buttons on purpose.
// Hold for WIFI_BUTTON_HOLD_MS to open the WiFiManager config portal
// — works both at boot and during normal runtime.
const int WIFI_SETUP_BUTTON_PIN = 14;
const unsigned long WIFI_BUTTON_HOLD_MS = 3000;
unsigned long wifiButtonPressStart = 0;
bool wifiButtonHeld = false;

// ---------- Temperature sensor (DS18B20) ----------
#define ONE_WIRE_PIN 15
OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature tempSensor(&oneWire);

// ---------- Connectivity LED timing ----------
const unsigned long STANDBY_LED_ON_MS = 10000;
const unsigned long STANDBY_LED_OFF_MS = 5000;
const unsigned long DB_FAILURE_FLASH_INTERVAL_MS = 5000;
const unsigned long DB_FAILURE_FLASH_DURATION_MS = 200;
unsigned long standbyLedCycleStart = 0;
unsigned long lastDbFailureFlash = 0;
unsigned long dbFailureFlashUntil = 0;
bool dbConnectionAvailable = true;

// ---------- Timings ----------
const unsigned long CHECK_DELAY  = 100;
const unsigned long HEARTBEAT_MS = 3000UL;

// ---------- Boot timing budgets (keep boot fast) ----------
const unsigned long BOOT_LINK_CHECK_MS    = 2000;  // physical link autonegotiation
const unsigned long BOOT_DHCP_TIMEOUT_MS  = 4000;  // bounded DHCP wait
const unsigned long BOOT_DHCP_RESPONSE_MS = 1000;
const unsigned long BOOT_WIFI_CONNECT_MS  = 5000;  // bounded WiFi wait at boot

// ---------- Lift state ----------
bool lastPressedA = false;
bool lastPressedB = false;
unsigned long lastHeartbeat = 0;

// ============================================================
//  CONNECTION MODE STATE MACHINE
//  ETHERNET        -> Ethernet active, WiFi connecting in background
//  MODE_DUAL       -> Ethernet active (primary) + WiFi hot standby ready
//  WIFI_CONNECTING -> Ethernet down, WiFi connecting (cold start)
//  WIFI            -> WiFi active (Ethernet unavailable)
// ============================================================
enum ConnMode { MODE_ETHERNET, MODE_DUAL, MODE_WIFI_CONNECTING, MODE_WIFI };
ConnMode connMode = MODE_ETHERNET;

// ---------- Runtime failover timings ----------
const unsigned long LINK_DOWN_GRACE_MS      = 3000;   // Ethernet must be down this long before failing over
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 10000;  // max wait for cold WiFi connect
const unsigned long WIFI_RETRY_COOLDOWN_MS  = 15000;  // cooldown before retrying a dropped WiFi

// ---------- Hot-standby WiFi timings ----------
const unsigned long WIFI_STANDBY_TIMEOUT_MS = 15000;  // max time to wait for background WiFi connect
const unsigned long WIFI_STANDBY_RETRY_MS   = 60000;  // retry standby WiFi after this cooldown
const unsigned long CONNECTION_PING_INTERVAL_MS = 30000;

unsigned long ethDownSince        = 0;
unsigned long wifiConnectStarted  = 0;
unsigned long lastWifiAttempt     = 0;

// Standby WiFi state
bool          wifiStandbyActive   = false;  // true = WiFi.begin() called as background standby
unsigned long wifiStandbyStart    = 0;      // when the standby connect was started
unsigned long lastConnectionPing = 0;
EthernetClient ethernetProbeClient;
WiFiClient wifiProbeClient;

const char* getStateName(int stateCode) {
  switch (stateCode) {
    case STATE_ALARM:   return "ALARM";
    case STATE_NORMAL:  return "NORMAL";
    case STATE_OFFLINE: return "OFFLINE";
    default:            return "UNKNOWN";
  }
}

void printUptime() {
  unsigned long sec = millis() / 1000;
  unsigned long min = sec / 60;
  unsigned long hr = min / 60;
  sec = sec % 60;
  min = min % 60;
  Serial.print(F("[Uptime: "));
  if (hr < 10) Serial.print('0');
  Serial.print(hr);
  Serial.print(':');
  if (min < 10) Serial.print('0');
  Serial.print(min);
  Serial.print(':');
  if (sec < 10) Serial.print('0');
  Serial.print(sec);
  Serial.print(F("] "));
}

// ============================================================
//  isLiftModuleConnected() — placeholder, see earlier note.
//  A plain pushbutton on INPUT_PULLUP can't distinguish "not
//  pressed" from "wire disconnected". Real detection needs extra
//  wiring (closed supervisory loop / heartbeat from a remote
//  module). TODO: implement once that hardware is decided.
// ============================================================
bool isLiftModuleConnected(int pin) {
  (void)pin;
  return true;
}

int getLiftState(bool pressed, bool moduleConnected) {
  if (!moduleConnected) return STATE_OFFLINE;
  return pressed ? STATE_ALARM : STATE_NORMAL;
}

int getTempStatus(float tempC, bool sensorOk) {
  if (!sensorOk) return STATE_OFFLINE;
  return (tempC > TEMP_HIGH_THRESHOLD_C) ? STATE_ALARM : STATE_NORMAL;
}

int getOverallStatus(int liftAState, int liftBState, int tempStatus) {
  if (liftAState == STATE_ALARM || liftBState == STATE_ALARM || tempStatus == STATE_ALARM) {
    return STATE_ALARM;
  }
  return STATE_NORMAL;
}

// ============================================================
//  getActiveClient()
//  Returns whichever transport is currently live, so the rest of
//  the code doesn't need to know or care which one it's using.
// ============================================================
Client* getActiveClient() {
  return (connMode == MODE_WIFI) ? (Client*)&wifiClient : (Client*)&ethClient;
}

unsigned long measureConnectionLatency(Client& probeClient) {
  unsigned long started = millis();
  if (!probeClient.connect(INFLUX_HOST, INFLUX_PORT)) {
    probeClient.stop();
    return 0xFFFFFFFFUL;
  }
  unsigned long latency = millis() - started;
  probeClient.stop();
  return latency;
}

void selectBestConnection(bool forceProbe) {
  if (Ethernet.linkStatus() != LinkON || WiFi.status() != WL_CONNECTED) return;
  if (!forceProbe && millis() - lastConnectionPing < CONNECTION_PING_INTERVAL_MS) return;
  lastConnectionPing = millis();

  unsigned long ethernetLatency = measureConnectionLatency(ethernetProbeClient);
  unsigned long wifiLatency = measureConnectionLatency(wifiProbeClient);
  printUptime();
  Serial.print(F("[NETWORK] DB latency - Ethernet: "));
  Serial.print(ethernetLatency == 0xFFFFFFFFUL ? -1 : (long)ethernetLatency);
  Serial.print(F(" ms, WiFi: "));
  Serial.print(wifiLatency == 0xFFFFFFFFUL ? -1 : (long)wifiLatency);
  Serial.println(F(" ms"));

  if (wifiLatency < ethernetLatency) {
    connMode = MODE_WIFI;
    digitalWrite(LED_WIFI, HIGH);
    Serial.println(F("[NETWORK] WiFi selected as lower-latency connection"));
  } else {
    connMode = MODE_DUAL;
    digitalWrite(LED_WIFI, HIGH);
    Serial.println(F("[NETWORK] Ethernet selected as lower-latency connection"));
  }
}

// ============================================================
//  postLineProtocol()
//  Shared HTTP POST helper, transport-agnostic.
// ============================================================
bool postLineProtocol(const char* payload) {
  Client* client = getActiveClient();

  if (!client->connect(INFLUX_HOST, INFLUX_PORT)) {
    dbConnectionAvailable = false;
    Serial.println(F("InfluxDB connect failed"));
    return false;
  }

  int len = strlen(payload);

  client->print(F("POST /api/v2/write?org="));
  client->print(INFLUX_ORG);
  client->print(F("&bucket="));
  client->print(INFLUX_BUCKET);
  client->println(F("&precision=s HTTP/1.1"));

  client->print(F("Host: "));
  client->println(INFLUX_HOST);

  client->print(F("Authorization: Token "));
  client->println(INFLUX_TOKEN);

  client->println(F("Content-Type: text/plain"));

  client->print(F("Content-Length: "));
  client->println(len);

  client->println(F("Connection: close"));
  client->println();
  client->print(payload);

  unsigned long t = millis();
  while (!client->available() && millis() - t < 1000) { /* wait */ }

  bool ok = false;
  if (client->available()) {
    char respBuf[32];
    int idx = 0;
    while (client->available() && idx < 31) {
      char c = client->read();
      if (c == '\n') break;
      respBuf[idx++] = c;
    }
    respBuf[idx] = '\0';

    if (strstr(respBuf, "204")) {
      ok = true;
      dbConnectionAvailable = true;
    } else {
      dbConnectionAvailable = false;
      Serial.print(F("InfluxDB error: "));
      Serial.println(respBuf);
    }
  } else {
    dbConnectionAvailable = false;
  }

  while (client->available()) client->read();
  client->stop();

  return ok;
}

bool readTemperatureC(float &tempOut) {
  tempSensor.requestTemperatures();
  float t = tempSensor.getTempCByIndex(0);
  if (t == DEVICE_DISCONNECTED_C) {
    Serial.println(F("Temp sensor disconnected/not found"));
    return false;
  }
  tempOut = t;
  return true;
}

void sendLiftEvent(const char* liftName, int stateCode) {
  char payload[80];
  snprintf(payload, sizeof(payload),
    "node_status,node=%s %s=%di",
    NODE_NAME, liftName, stateCode);
  postLineProtocol(payload);
}

void sendHeartbeat() {
  bool connA = isLiftModuleConnected(liftAPin);
  bool connB = isLiftModuleConnected(liftBPin);
  int stateA = getLiftState(lastPressedA, connA);
  int stateB = getLiftState(lastPressedB, connB);

  float tempC = 0;
  bool tempOk = readTemperatureC(tempC);
  int tempStatus = getTempStatus(tempC, tempOk);

  int overall = getOverallStatus(stateA, stateB, tempStatus);

  char payload[250];
  if (tempOk) {
    snprintf(payload, sizeof(payload),
      "node_status,node=%s temp=%.2f,%s=%di,%s=%di",
      NODE_NAME, tempC, LIFT_A, stateA, LIFT_B, stateB);
  } else {
    snprintf(payload, sizeof(payload),
      "node_status,node=%s temp=0.00,%s=%di,%s=%di",
      NODE_NAME, LIFT_A, stateA, LIFT_B, stateB);
  }

  postLineProtocol(payload);

  // -- Connection status block --
  printUptime();
  Serial.println(F("[HEARTBEAT] --- Network ---"));

  // Ethernet
  printUptime();
  Serial.print(F("  ETH  : "));
  if (Ethernet.linkStatus() == LinkON) {
    if (connMode == MODE_ETHERNET) {
      Serial.print(F("UP (active) IP: "));
      Serial.println(Ethernet.localIP());
    } else {
      Serial.print(F("UP (standby) IP: "));
      Serial.println(Ethernet.localIP());
    }
  } else {
    Serial.println(F("LINK DOWN"));
  }

  // WiFi
  printUptime();
  Serial.print(F("  WiFi : "));
  if (connMode == MODE_WIFI && WiFi.status() == WL_CONNECTED) {
    Serial.print(F("CONNECTED (active) IP: "));
    Serial.println(WiFi.localIP());
  } else if (connMode == MODE_WIFI_CONNECTING) {
    Serial.println(F("CONNECTING..."));
  } else if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("CONNECTED (standby) IP: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("OFF / NOT CONNECTED"));
  }

  // -- Sensor and lift status --
  printUptime();
  Serial.println(F("[HEARTBEAT] --- Status ---"));
  printUptime();
  Serial.print(F("  "));
  Serial.print(LIFT_A);
  Serial.print(F(": "));
  Serial.print(getStateName(stateA));
  Serial.print(F("  |  "));
  Serial.print(LIFT_B);
  Serial.print(F(": "));
  Serial.print(getStateName(stateB));
  Serial.print(F("  |  Temp: "));
  if (tempOk) {
    Serial.print(tempC, 1);
    Serial.print(F("C"));
  } else {
    Serial.print(F("FAULT"));
  }
  Serial.print(F(" ("));
  Serial.print(getStateName(tempStatus));
  Serial.print(F(")  |  Overall: "));
  Serial.println(getStateName(overall));
}

// ---------- Ethernet activity LED ----------
void updateEthernetLed() {
  bool ethernetConnected = (Ethernet.linkStatus() == LinkON);
  bool wifiConnected = (WiFi.status() == WL_CONNECTED);
  bool ethernetActive = ethernetConnected && (connMode == MODE_ETHERNET || connMode == MODE_DUAL);
  bool wifiActive = wifiConnected && (connMode == MODE_WIFI);
  bool ethernetStandby = ethernetConnected && !ethernetActive;
  bool wifiStandby = wifiConnected && !wifiActive;
  unsigned long now = millis();

  if (ethernetStandby || wifiStandby) {
    if (standbyLedCycleStart == 0) standbyLedCycleStart = now;
  } else {
    standbyLedCycleStart = 0;
  }

  bool standbyLedOn = false;
  if (standbyLedCycleStart != 0) {
    unsigned long cycleElapsed = (now - standbyLedCycleStart) %
                                 (STANDBY_LED_ON_MS + STANDBY_LED_OFF_MS);
    standbyLedOn = cycleElapsed < STANDBY_LED_ON_MS;
  }

  bool dbFailureFlash = false;
  if (!dbConnectionAvailable) {
    if (now - lastDbFailureFlash >= DB_FAILURE_FLASH_INTERVAL_MS) {
      lastDbFailureFlash = now;
      dbFailureFlashUntil = now + DB_FAILURE_FLASH_DURATION_MS;
    }
    dbFailureFlash = now < dbFailureFlashUntil;
  }

  // Active connection is solid when the database is reachable. If the
  // database cannot be reached, show only the five-second quick flash.
  digitalWrite(LED_ETH, ethernetActive ? (dbConnectionAvailable ? HIGH : (dbFailureFlash ? HIGH : LOW)) : LOW);
  digitalWrite(LED_WIFI, wifiActive ? (dbConnectionAvailable ? HIGH : (dbFailureFlash ? HIGH : LOW)) : LOW);

  // Connected standby connection: 10 seconds ON, then 5 seconds OFF.
  if (ethernetStandby) digitalWrite(LED_ETH, standbyLedOn ? HIGH : LOW);
  if (wifiStandby) digitalWrite(LED_WIFI, standbyLedOn ? HIGH : LOW);
}

// ============================================================
//  updateConnectionMode()
//  Dual-connection hot standby state machine.
//  - Ethernet wins when both paths have equal latency.
//  - WiFi is kept connected as a silent hot standby when Ethernet is active.
//  - When Ethernet fails: INSTANT switch to pre-connected WiFi.
//  Called once per loop() iteration.
// ============================================================
void updateConnectionMode() {
  bool ethLinkUp     = (Ethernet.linkStatus() == LinkON);
  bool wifiConnected = (WiFi.status() == WL_CONNECTED);

  switch (connMode) {

    // ---- Ethernet active, background WiFi connecting ----
    case MODE_ETHERNET: {
      if (ethLinkUp) {
        ethDownSince = 0;
        // Promote to DUAL if background WiFi just finished connecting
        if (wifiConnected) {
          selectBestConnection(true);
          wifiStandbyActive = false;
          digitalWrite(LED_WIFI, HIGH);
          printUptime();
          Serial.print(F("[NETWORK] WiFi hot standby ready. IP: "));
          Serial.println(WiFi.localIP());
          return;
        }
        // Start or maintain background WiFi connection attempt
        if (!wifiStandbyActive) {
          if (lastWifiAttempt == 0 || millis() - lastWifiAttempt >= WIFI_STANDBY_RETRY_MS) {
            WiFi.disconnect(true); delay(50);
            WiFi.mode(WIFI_STA);
            WiFi.begin();
            wifiStandbyActive = true;
            wifiStandbyStart  = millis();
          }
        } else if (millis() - wifiStandbyStart >= WIFI_STANDBY_TIMEOUT_MS) {
          // Background connect timed out - pause before retry
          WiFi.disconnect(true);
          wifiStandbyActive = false;
          lastWifiAttempt   = millis();
        }
        return;
      }
      // Ethernet down
      if (ethDownSince == 0) { ethDownSince = millis(); return; }
      if (millis() - ethDownSince >= LINK_DOWN_GRACE_MS) {
        if (wifiConnected) {
          // INSTANT switch - WiFi was already connected as standby
          printUptime();
          Serial.println(F("[NETWORK] Ethernet down - INSTANT switch to WiFi hot standby (0ms gap)"));
          connMode = MODE_WIFI;
          wifiStandbyActive = false;
          return;
        }
        // Cold start WiFi
        if (lastWifiAttempt != 0 && millis() - lastWifiAttempt < WIFI_RETRY_COOLDOWN_MS) return;
        printUptime();
        Serial.println(F("[NETWORK] Ethernet down - starting WiFi (cold)..."));
        WiFi.disconnect(true); delay(50);
        WiFi.mode(WIFI_STA);
        WiFi.begin();
        wifiConnectStarted = millis();
        lastWifiAttempt    = millis();
        wifiStandbyActive  = false;
        connMode = MODE_WIFI_CONNECTING;
      }
      break;
    }

    // ---- Ethernet active (primary) + WiFi connected (hot standby) ----
    case MODE_DUAL: {
      if (ethLinkUp) {
        ethDownSince = 0;
        if (!wifiConnected) {
          // Standby WiFi dropped - demote and restart it
          printUptime();
          Serial.println(F("[NETWORK] WiFi hot standby lost - reconnecting in background..."));
          connMode = MODE_ETHERNET;
          wifiStandbyActive = false;
          lastWifiAttempt   = millis() - WIFI_STANDBY_RETRY_MS;  // allow immediate retry
          digitalWrite(LED_WIFI, LOW);
        } else {
          selectBestConnection(false);
        }
        if (connMode == MODE_WIFI) return;
        return;
      }
      // Ethernet went down
      if (ethDownSince == 0) { ethDownSince = millis(); return; }
      if (millis() - ethDownSince >= LINK_DOWN_GRACE_MS) {
        if (wifiConnected) {
          // INSTANT failover - no reconnect delay, zero data loss
          printUptime();
          Serial.println(F("[NETWORK] Ethernet down - INSTANT failover to WiFi hot standby"));
          connMode = MODE_WIFI;
          return;
        }
        // WiFi also dropped - cold start
        printUptime();
        Serial.println(F("[NETWORK] Both interfaces down - connecting WiFi..."));
        WiFi.disconnect(true); delay(50);
        WiFi.mode(WIFI_STA);
        WiFi.begin();
        wifiConnectStarted = millis();
        lastWifiAttempt    = millis();
        wifiStandbyActive  = false;
        connMode = MODE_WIFI_CONNECTING;
      }
      break;
    }

    // ---- WiFi connecting (cold start after Ethernet failed) ----
    case MODE_WIFI_CONNECTING: {
      if (ethLinkUp) {
        // Ethernet came back - use it, let WiFi connect become the standby
        printUptime();
        Serial.println(F("[NETWORK] Ethernet restored - switching back, WiFi will become standby"));
        connMode = MODE_ETHERNET;
        ethDownSince = 0;
        // Don't disconnect WiFi - let it finish and become standby
        wifiStandbyActive = true;
        wifiStandbyStart  = millis();
        return;
      }
      if (wifiConnected) {
        connMode = MODE_WIFI;
        digitalWrite(LED_WIFI, HIGH);
        printUptime();
        Serial.print(F("[NETWORK] WiFi connected. IP: "));
        Serial.println(WiFi.localIP());
        return;
      }
      if (millis() - wifiConnectStarted >= WIFI_CONNECT_TIMEOUT_MS) {
        printUptime();
        Serial.println(F("[NETWORK] WiFi connect timed out - will retry after cooldown"));
        WiFi.disconnect(true);
        connMode = MODE_ETHERNET;
        lastWifiAttempt = millis();
        ethDownSince    = millis();
      }
      break;
    }

    // ---- WiFi active (Ethernet unavailable) ----
    case MODE_WIFI: {
      if (ethLinkUp) {
        if (wifiConnected) {
          selectBestConnection(false);
          if (connMode == MODE_WIFI) return;
        }
        // Ethernet back - it becomes primary, WiFi becomes hot standby
        printUptime();
        Serial.println(F("[NETWORK] Ethernet restored - switching back (WiFi stays as hot standby)"));
        connMode     = wifiConnected ? MODE_DUAL : MODE_ETHERNET;
        ethDownSince = 0;
        if (!wifiConnected) {
          wifiStandbyActive = false;
          digitalWrite(LED_WIFI, LOW);
        }
        return;
      }
      if (!wifiConnected) {
        if (millis() - lastWifiAttempt >= WIFI_RETRY_COOLDOWN_MS) {
          printUptime();
          Serial.println(F("[NETWORK] WiFi dropped - retrying..."));
          WiFi.disconnect(true); delay(50);
          WiFi.begin();
          wifiConnectStarted = millis();
          lastWifiAttempt    = millis();
          connMode = MODE_WIFI_CONNECTING;
          digitalWrite(LED_WIFI, LOW);
        }
      }
      break;
    }
  }
}

// ============================================================
//  checkWifiSetupButton()  [runtime]
//  Called every loop(). Hold GPIO14 for WIFI_BUTTON_HOLD_MS to
//  open the config portal. Interrupts all other activity.
//  After the portal exits, updateConnectionMode() re-establishes
//  the best available connection automatically.
// ============================================================
void checkWifiSetupButton() {
  bool pressed = !digitalRead(WIFI_SETUP_BUTTON_PIN);
  if (pressed) {
    if (wifiButtonPressStart == 0) {
      wifiButtonPressStart = millis();
    } else if (!wifiButtonHeld && millis() - wifiButtonPressStart >= WIFI_BUTTON_HOLD_MS) {
      wifiButtonHeld = true;
      printUptime();
      Serial.println(F("[BUTTON] WiFi setup button held - launching config portal"));
      startWifiConfigPortal();
      // After portal: force updateConnectionMode() to re-evaluate on next loop
      ethDownSince = 1;
      wifiButtonHeld   = false;
      wifiButtonPressStart = 0;
    }
  } else {
    wifiButtonPressStart = 0;
    wifiButtonHeld = false;
  }
}

// ============================================================
//  bootCheckWifiButtonHeld()
//  Same button-hold check as checkWifiSetupButton(), but usable
//  inside the bounded boot-time waits below so a held button
//  interrupts BOOT connection attempts too, not just runtime ones.
//  Returns true if the portal was triggered (caller should stop
//  what it was doing and return, since the portal already ran).
// ============================================================
bool bootCheckWifiButtonHeld() {
  bool pressed = !digitalRead(WIFI_SETUP_BUTTON_PIN);
  if (!pressed) {
    wifiButtonPressStart = 0;
    return false;
  }
  if (wifiButtonPressStart == 0) {
    wifiButtonPressStart = millis();
    return false;
  }
  if (millis() - wifiButtonPressStart >= WIFI_BUTTON_HOLD_MS) {
    printUptime();
    Serial.println(F("[BOOT] WiFi setup button held - interrupting boot for config portal"));
    startWifiConfigPortal();
    wifiButtonPressStart = 0;
    wifiButtonHeld = false;
    return true;
  }
  return false;
}

// ---------- Config portal customization ----------
const char* AP_NAME_VALUE     = AP_NAME;
const char* AP_PASSWORD_VALUE = AP_PASSWORD;

WiFiManagerParameter portalInfo(
  "<p><b>Lift Alarm Node - WiFi Fallback Setup</b><br>"
  "Enter the WiFi network this node should use if its Ethernet "
  "cable is ever unplugged or the switch/router goes down.</p>"
);

void onWifiConfigSaved() {
  Serial.println(F("WiFi credentials received from portal - saving"));
}

// ---------- WiFi configuration LED blink ----------
Ticker wifiLedTicker;
void tickWifiLed() {
  digitalWrite(LED_WIFI, !digitalRead(LED_WIFI));
}

// ============================================================
//  startWifiConfigPortal()
//  Blocking by design — only runs when someone deliberately holds
//  the setup button (at boot or during runtime), so pausing
//  everything else for the duration of manual configuration is an
//  accepted trade-off.
// ============================================================
void startWifiConfigPortal() {
  Serial.println(F("WiFi setup button held - starting config portal"));
  wifiLedTicker.attach_ms(250, tickWifiLed);

  WiFiManager wm;

  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(20);
  wm.setBreakAfterConfig(true);

  wm.setTitle("Lift Alarm Node");
  wm.setClass("invert");
  wm.setShowInfoUpdate(true);
  wm.setShowInfoErase(false);
  wm.addParameter(&portalInfo);

  std::vector<const char*> menu = { "wifi", "info", "exit" };
  wm.setMenu(menu);

  wm.setSaveConfigCallback(onWifiConfigSaved);

  bool connected = wm.startConfigPortal(AP_NAME_VALUE, AP_PASSWORD_VALUE);
  wifiLedTicker.detach();

  if (connected) {
    Serial.println(F("WiFi credentials saved and connected."));
    connMode = MODE_WIFI;
    digitalWrite(LED_WIFI, HIGH);
  } else {
    Serial.println(F("WiFi config portal timed out or failed."));
    digitalWrite(LED_WIFI, LOW);
  }
  // Don't force any further mode change here — updateConnectionMode()
  // (or bootConnect(), if this was called during boot) picks up
  // Ethernet-vs-WiFi normally on the next check.
}

// ============================================================
//  bootConnect()
//  Fast boot connection race:
//    1. Wait up to BOOT_LINK_CHECK_MS for Ethernet physical link.
//    2. If link found: try DHCP (bounded), then static IP fallback.
//    3. If Ethernet not ready: immediately try WiFi for BOOT_WIFI_CONNECT_MS.
//    4. If neither: hand off to loop() to keep retrying in background.
//  WiFi-setup button can interrupt at any wait point.
// ============================================================
void bootConnect() {
  // ---- Stage 1: wait for Ethernet physical link ----
  Serial.println(F("[BOOT] Checking Ethernet physical link..."));
  unsigned long t = millis();
  bool linkUp = (Ethernet.linkStatus() == LinkON);
  while (!linkUp && millis() - t < BOOT_LINK_CHECK_MS) {
    if (bootCheckWifiButtonHeld()) return;
    delay(100);
    linkUp = (Ethernet.linkStatus() == LinkON);
  }

  if (linkUp) {
    // ---- Stage 2a: Ethernet link present - try DHCP (bounded) ----
    Serial.println(F("[BOOT] Ethernet link up - requesting IP via DHCP..."));
    int dhcpOk = Ethernet.begin(mac, BOOT_DHCP_TIMEOUT_MS, BOOT_DHCP_RESPONSE_MS);
    if (dhcpOk) {
      connMode = MODE_ETHERNET;
      ethDownSince = 0;
      printUptime();
      Serial.print(F("[BOOT] Ethernet ready (DHCP). IP: "));
      Serial.println(Ethernet.localIP());
      // Start WiFi hot standby in background (non-blocking)
      Serial.println(F("[BOOT] Starting WiFi hot standby in background..."));
      WiFi.mode(WIFI_STA);
      WiFi.begin();
      wifiStandbyActive = true;
      wifiStandbyStart  = millis();
      return;
    }
    // ---- Stage 2b: DHCP timed out - fall back to static IP (instant) ----
    Serial.println(F("[BOOT] DHCP timed out - applying static IP"));
    Ethernet.begin(mac, ip);
    if (Ethernet.linkStatus() == LinkON) {
      connMode = MODE_ETHERNET;
      ethDownSince = 0;
      printUptime();
      Serial.print(F("[BOOT] Ethernet ready (static IP): "));
      Serial.println(ip);
      // Start WiFi hot standby in background (non-blocking)
      Serial.println(F("[BOOT] Starting WiFi hot standby in background..."));
      WiFi.mode(WIFI_STA);
      WiFi.begin();
      wifiStandbyActive = true;
      wifiStandbyStart  = millis();
      return;
    }
    Serial.println(F("[BOOT] Ethernet link lost after static IP assign - trying WiFi"));
  } else {
    Serial.println(F("[BOOT] No Ethernet link - trying WiFi immediately"));
  }

  // ---- Stage 3: try WiFi (saved credentials) ----
  Serial.println(F("[BOOT] Connecting to saved WiFi network..."));
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  t = millis();
  while (millis() - t < BOOT_WIFI_CONNECT_MS) {
    if (bootCheckWifiButtonHeld()) return;
    if (WiFi.status() == WL_CONNECTED) {
      connMode = MODE_WIFI;
      digitalWrite(LED_WIFI, HIGH);
      printUptime();
      Serial.print(F("[BOOT] ✅ WiFi connected. IP: "));
      Serial.println(WiFi.localIP());
      return;
    }
    delay(100);
  }

  // ---- Stage 4: neither connected in time - hand off to loop() ----
  Serial.println(F("[BOOT] No connection in boot window - loop() will keep retrying in background."));
  connMode = MODE_ETHERNET;
  ethDownSince    = millis() - LINK_DOWN_GRACE_MS + 1;  // trigger fast WiFi fallback on 1st loop()
  lastWifiAttempt = 0;
}

// ============================================================
void setup() {
  Serial.begin(115200);

  // -- GPIO --
  pinMode(LED_POWER, OUTPUT); digitalWrite(LED_POWER, HIGH);
  pinMode(LED_ETH,   OUTPUT); digitalWrite(LED_ETH,   LOW);
  pinMode(LED_WIFI,  OUTPUT); digitalWrite(LED_WIFI,  LOW);
  pinMode(liftAPin,          INPUT_PULLUP);
  pinMode(liftBPin,          INPUT_PULLUP);
  pinMode(WIFI_SETUP_BUTTON_PIN, INPUT_PULLUP);

  // -- W5500 hardware reset (clears any stale link state) --
  pinMode(W5500_RST, OUTPUT);
  digitalWrite(W5500_RST, LOW);  delay(50);
  digitalWrite(W5500_RST, HIGH); delay(200);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, W5500_CS);
  Ethernet.init(W5500_CS);
  // Pre-assign static IP so Ethernet.linkStatus() works before DHCP
  Ethernet.begin(mac, ip);

  // -- Startup banner --
  Serial.println(F("\n=================================================="));
  Serial.print  (F("  SLT EMERGENCY ALARM SYSTEM  |  NODE: "));
  Serial.println(NODE_NAME);
  Serial.println(F("=================================================="));

  // -- Fast-boot connection race (Ethernet first, WiFi fallback) --
  bootConnect();

  Serial.print(F("[INIT] Active connection: "));
  if      (connMode == MODE_WIFI)           Serial.println(F("WiFi"));
  else if (connMode == MODE_DUAL)           Serial.println(F("Ethernet + WiFi (hot standby)"));
  else if (Ethernet.linkStatus() == LinkON) Serial.println(F("Ethernet (WiFi hot standby connecting...)"));
  else                                      Serial.println(F("None - retrying in background"));

  // -- Sensors --
  tempSensor.begin();
  Serial.print(F("[INIT] DS18B20 sensor count: "));
  Serial.println(tempSensor.getDeviceCount());

  // -- Initial lift states --
  lastPressedA = !digitalRead(liftAPin);
  lastPressedB = !digitalRead(liftBPin);

  Serial.println(F("[INIT] Boot complete - uploading initial status..."));
  Serial.println(F("==================================================\n"));

  // -- First heartbeat: read lift + temp, upload to DB --
  sendHeartbeat();
}

// ============================================================
void loop() {
  updateConnectionMode();
  updateEthernetLed();
  checkWifiSetupButton();   // interrupts runtime at any point if held

  bool curA = !digitalRead(liftAPin);
  bool curB = !digitalRead(liftBPin);

  if (curA != lastPressedA) {
    lastPressedA = curA;
    int stateA = getLiftState(lastPressedA, isLiftModuleConnected(liftAPin));
    sendLiftEvent(LIFT_A, stateA);
    printUptime();
    Serial.print(F("[EVENT] State changed — "));
    Serial.print(LIFT_A);
    Serial.print(F(": "));
    Serial.println(getStateName(stateA));
  }

  if (curB != lastPressedB) {
    lastPressedB = curB;
    int stateB = getLiftState(lastPressedB, isLiftModuleConnected(liftBPin));
    sendLiftEvent(LIFT_B, stateB);
    printUptime();
    Serial.print(F("[EVENT] State changed — "));
    Serial.print(LIFT_B);
    Serial.print(F(": "));
    Serial.println(getStateName(stateB));
  }

  if (millis() - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = millis();
    sendHeartbeat();
  }

  delay(CHECK_DELAY);
}
