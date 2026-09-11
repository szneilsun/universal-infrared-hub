// ESP32-S3 infrared Hub: common hardware, HomeKit, and learning console.

#include <IRremote.hpp>
#include <Preferences.h>
#include <HomeSpan.h>
#include <WebServer.h>
#include <WiFi.h>
#include <nvs.h>
#include "dynamic_devices.h"
#include "home_background.h"

#ifndef IR_CAPTURE_VERBOSE
#define IR_CAPTURE_VERBOSE 1
#endif

// ESP32-S3 Dev Module wiring: receiver OUT -> GPIO 41, transmitter driver -> GPIO 40.
constexpr uint8_t IR_RECEIVE_PIN = 41;
constexpr uint8_t IR_SEND_PIN = 40;
constexpr uint8_t MAX_CODES = 20;
constexpr uint32_t CODE_MAGIC = 0x49524332;
constexpr uint32_t HUB_AID = 1;
constexpr uint32_t AIR_CONDITIONER_AID = 2;
constexpr uint32_t TELEVISION_AID = 3;
constexpr uint32_t USER_DEVICE_AID_BASE = 4;
constexpr char FIRMWARE_VERSION[] = "2.1.9";
constexpr char FIRMWARE_BUILD_DATE[] = __DATE__ " " __TIME__;
constexpr char AP_SSID[] = "IR-AC-Setup";
constexpr char AP_PASSWORD[] = "iracsetup";
constexpr char HOMEKIT_PAIRING_CODE[] = "11122333";
constexpr uint16_t AP_SETUP_TIMEOUT_SECONDS = 300;
constexpr uint16_t HOMEKIT_PORT = 51826;
constexpr uint8_t MAX_WIFI_PROFILES = 5;
constexpr uint32_t WIFI_PROFILES_MAGIC = 0x57494636;  // "WIF6"

struct __attribute__((packed)) LearnedCode {
  uint32_t magic;
  uint8_t protocol;
  uint16_t address;
  uint16_t command;
  uint16_t extra;
  uint64_t rawCode;
  uint16_t bits;
};

enum ConsoleMode : uint8_t {
  IDLE, LEARN_SELECT_SLOT, LEARN_WAIT_IR, SEND_SELECT_SLOT
};

Preferences preferences;
Preferences wifiProfilePreferences;
ConsoleMode consoleMode = IDLE;
int8_t pendingSlot = -1;
char serialLine[32] = {};
uint8_t serialLineLength = 0;
bool irCaptureVerbose = IR_CAPTURE_VERBOSE;
uint32_t lastIrTransmissionFinishedAt = 0;

struct __attribute__((packed)) WiFiProfile {
  char ssid[33];
  char password[65];
};

struct __attribute__((packed)) WiFiProfileStore {
  uint32_t magic;
  WiFiProfile profiles[MAX_WIFI_PROFILES];
};

WiFiProfileStore wifiProfileStore = {};
WebServer provisioningServer(80);
bool provisioningActive = false;
uint32_t provisioningEndsAt = 0;
uint32_t nextProvisioningStatusAt = 0;

void configureAirConditionerAccessory();
void configureTelevisionAccessory();
void configureUserDevices();
void beginDeviceManager();
void pollDeviceManager();
bool isWebLearning();
void saveWebLearnedCode(const IRData &signal);
void resetNetworkConfiguration();
void resetHomeKitConfiguration();
uint8_t cycleSetTopBoxCarrier();
uint8_t getSetTopBoxCarrier();
void startSetupHotspot();
void pollSetupHotspot();

bool isValidWiFiProfile(const WiFiProfile &profile) {
  return profile.ssid[0] != '\0';
}

void saveWiFiProfiles() {
  wifiProfilePreferences.putBytes("profiles", &wifiProfileStore,
                                  sizeof(wifiProfileStore));
}

void loadWiFiProfiles() {
  wifiProfilePreferences.begin("wifi-profiles", false);
  if (wifiProfilePreferences.getBytesLength("profiles") == sizeof(wifiProfileStore)) {
    wifiProfilePreferences.getBytes("profiles", &wifiProfileStore,
                                    sizeof(wifiProfileStore));
  }
  if (wifiProfileStore.magic != WIFI_PROFILES_MAGIC) {
    memset(&wifiProfileStore, 0, sizeof(wifiProfileStore));
    wifiProfileStore.magic = WIFI_PROFILES_MAGIC;
  }
}

void addWiFiProfile(const char *ssid, const char *password) {
  if (ssid == nullptr || ssid[0] == '\0') return;
  int freeIndex = -1;
  for (uint8_t i = 0; i < MAX_WIFI_PROFILES; ++i) {
    WiFiProfile &profile = wifiProfileStore.profiles[i];
    if (strcmp(profile.ssid, ssid) == 0) {
      snprintf(profile.password, sizeof(profile.password), "%s", password);
      saveWiFiProfiles();
      return;
    }
    if (!isValidWiFiProfile(profile) && freeIndex < 0) freeIndex = i;
  }
  // If the list is full, replace the final entry with the newly configured Wi-Fi.
  WiFiProfile &profile = wifiProfileStore.profiles[
      freeIndex >= 0 ? freeIndex : MAX_WIFI_PROFILES - 1];
  snprintf(profile.ssid, sizeof(profile.ssid), "%s", ssid);
  snprintf(profile.password, sizeof(profile.password), "%s", password);
  saveWiFiProfiles();
}

void migrateExistingHomeSpanWiFi() {
  struct __attribute__((packed)) HomeSpanWiFiData {
    char ssid[33];
    char password[65];
  } existing = {};
  nvs_handle handle;
  size_t size = sizeof(existing);
  if (nvs_open("WIFI", NVS_READONLY, &handle) == ESP_OK) {
    if (nvs_get_blob(handle, "WIFIDATA", &existing, &size) == ESP_OK &&
        size == sizeof(existing) && existing.ssid[0] != '\0') {
      addWiFiProfile(existing.ssid, existing.password);
    }
    nvs_close(handle);
  }
}

void selectReachableWiFi() {
  int bestProfile = -1;
  int bestRssi = -1000;
  WiFi.mode(WIFI_STA);
  Serial.println("Scanning for saved Wi-Fi networks...");
  int networkCount = WiFi.scanNetworks();
  for (int network = 0; network < networkCount; ++network) {
    String ssid = WiFi.SSID(network);
    for (uint8_t profile = 0; profile < MAX_WIFI_PROFILES; ++profile) {
      if (isValidWiFiProfile(wifiProfileStore.profiles[profile]) &&
          ssid == wifiProfileStore.profiles[profile].ssid &&
          WiFi.RSSI(network) > bestRssi) {
        bestProfile = profile;
        bestRssi = WiFi.RSSI(network);
      }
    }
  }
  WiFi.scanDelete();
  if (bestProfile >= 0) {
    const WiFiProfile &profile = wifiProfileStore.profiles[bestProfile];
    homeSpan.setWifiCredentials(profile.ssid, profile.password);
    Serial.printf("Selected saved Wi-Fi: %s (RSSI %d dBm)\n", profile.ssid, bestRssi);
  } else {
    Serial.println("No saved Wi-Fi network is currently in range.");
  }
}

String escapeHtml(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    switch (value[i]) {
      case '&': escaped += F("&amp;"); break;
      case '<': escaped += F("&lt;"); break;
      case '>': escaped += F("&gt;"); break;
      case '\"': escaped += F("&quot;"); break;
      case '\'': escaped += F("&#39;"); break;
      default: escaped += value[i]; break;
    }
  }
  return escaped;
}

void sendProvisioningPage() {
  String knownNetworks;
  for (uint8_t i = 0; i < MAX_WIFI_PROFILES; ++i) {
    if (isValidWiFiProfile(wifiProfileStore.profiles[i])) {
      knownNetworks += "<li>" + escapeHtml(wifiProfileStore.profiles[i].ssid) + "</li>";
    }
  }
  if (knownNetworks.length() == 0) knownNetworks = "<li>尚未保存网络</li>";

  String networkOptions = "<option value='' selected disabled>请选择 Wi-Fi 热点</option>";
  Serial.println("Provisioning page: scanning nearby Wi-Fi networks...");
  int networkCount = WiFi.scanNetworks();
  for (int network = 0; network < networkCount; ++network) {
    String ssid = WiFi.SSID(network);
    if (ssid.length() == 0) continue;
    bool duplicate = false;
    for (int previous = 0; previous < network; ++previous) {
      if (ssid == WiFi.SSID(previous)) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    String safeSsid = escapeHtml(ssid);
    networkOptions += "<option value=\"" + safeSsid + "\">" + safeSsid + " (" +
                      String(WiFi.RSSI(network)) + " dBm)";
    if (WiFi.encryptionType(network) != WIFI_AUTH_OPEN) networkOptions += " &#128274;";
    networkOptions += "</option>";
  }
  WiFi.scanDelete();
  if (networkCount <= 0) {
    networkOptions += "<option value='' disabled>未扫描到热点，可手动输入</option>";
  }

  String page = "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>红外 Hub 网络设置</title><style>body{font-family:sans-serif;max-width:480px;margin:36px auto;padding:0 20px;line-height:1.5}input,select,button{box-sizing:border-box;width:100%;padding:12px;margin:7px 0;font-size:16px}button{background:#285c45;color:white;border:0;border-radius:8px}.hint{color:#667;font-size:14px}</style>"
                "<h1>红外 Hub 网络设置</h1><p>保存后会保留已有网络（最多 5 个），设备会自动连接当前可用且信号最强的网络。</p>"
                "<form method='post' action='/save'><select name='ssid'>" + networkOptions + "</select>"
                "<input name='manualSsid' maxlength='32' placeholder='隐藏网络：手动输入 SSID（可选）'>"
                "<input name='password' type='password' maxlength='64' placeholder='Wi-Fi 密码'>"
                "<button>保存并连接</button></form><p class='hint'>没看到热点？<a href='/'>重新扫描</a></p><h2>已保存网络</h2><ul>" + knownNetworks + "</ul>";
  provisioningServer.send(200, "text/html; charset=utf-8", page);
}

void saveProvisionedWiFi() {
  String ssid = provisioningServer.arg("ssid");
  String manualSsid = provisioningServer.arg("manualSsid");
  String password = provisioningServer.arg("password");
  ssid.trim();
  manualSsid.trim();
  if (manualSsid.length() > 0) ssid = manualSsid;
  if (ssid.length() == 0) {
    provisioningServer.send(400, "text/plain; charset=utf-8", "Wi-Fi 名称不能为空");
    return;
  }
  addWiFiProfile(ssid.c_str(), password.c_str());
  homeSpan.setWifiCredentials(ssid.c_str(), password.c_str());
  provisioningServer.send(200, "text/html; charset=utf-8",
                          "<meta http-equiv='refresh' content='3'><p>网络已保存，设备正在重启并连接。</p>");
  delay(800);
  ESP.restart();
}

void startSetupHotspot() {
  if (provisioningActive) return;
  // STA mode is kept enabled so the setup page can scan nearby access points.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  provisioningServer.on("/", HTTP_GET, sendProvisioningPage);
  provisioningServer.on("/save", HTTP_POST, saveProvisionedWiFi);
  provisioningServer.begin();
  provisioningActive = true;
  provisioningEndsAt = millis() + AP_SETUP_TIMEOUT_SECONDS * 1000UL;
  nextProvisioningStatusAt = millis() + 10000;
  Serial.printf("Setup hotspot %s is available for %u minutes at http://192.168.4.1\n",
                AP_SSID, AP_SETUP_TIMEOUT_SECONDS / 60);
}

void pollSetupHotspot() {
  if (!provisioningActive) return;
  provisioningServer.handleClient();
  if ((int32_t)(millis() - nextProvisioningStatusAt) >= 0) {
    Serial.printf("Setup hotspot active at http://192.168.4.1 (%s); STA=%s\n",
                  AP_SSID, WiFi.status() == WL_CONNECTED ? "connected" : "connecting");
    nextProvisioningStatusAt = millis() + 10000;
  }
  if ((int32_t)(millis() - provisioningEndsAt) < 0) return;

  provisioningServer.stop();
  WiFi.softAPdisconnect(false);
  WiFi.mode(WIFI_STA);
  provisioningActive = false;
  Serial.println("Setup hotspot closed after 5 minutes; station Wi-Fi remains active.");
}

void announceNetworkConnection(int count) {
  String ip = WiFi.localIP().toString();
  Serial.printf("\n=== IR Hub network ready (connection %d) ===\n", count);
  Serial.printf("Open: http://ir-hub.local:8080\n");
  Serial.printf("IPv4: http://%s:8080\n", ip.c_str());
}

void waitForIrTransmitter() {
  constexpr uint32_t MINIMUM_QUIET_TIME_MS = 350;
  uint32_t elapsed = millis() - lastIrTransmissionFinishedAt;
  if (elapsed < MINIMUM_QUIET_TIME_MS) delay(MINIMUM_QUIET_TIME_MS - elapsed);
  // Do not let the receiver decode our own LED while a long raw frame is sent.
  // On ESP32-S3 this also prevents receive interrupts from disturbing timing.
  IrReceiver.stop();
}

void finishIrTransmission() {
  delay(5);
  IrReceiver.start();
  lastIrTransmissionFinishedAt = millis();
}

void recordKey(uint8_t slot, char *key, size_t keySize) {
  snprintf(key, keySize, "code%u", slot);
}

bool loadCode(uint8_t slot, LearnedCode &code) {
  char key[12];
  recordKey(slot, key, sizeof(key));
  if (preferences.getBytesLength(key) != sizeof(LearnedCode)) return false;
  preferences.getBytes(key, &code, sizeof(code));
  return code.magic == CODE_MAGIC;
}

bool isSupportedForStorage(const IRData &signal) {
  return signal.protocol != UNKNOWN && signal.protocol != PULSE_DISTANCE &&
         signal.protocol != PULSE_WIDTH;
}

void printMainMenu() {
  Serial.println("\n=== Universal IR Remote ===");
  Serial.println("1 - Learn IR commands");
  Serial.println("2 - Send saved commands");
  Serial.println("3 - Version information");
  Serial.println("4 - Reset HomeKit and Wi-Fi setup");
  Serial.printf("5 - Toggle full IR timings (currently %s)\n",
                irCaptureVerbose ? "ON" : "OFF");
  Serial.printf("6 - Cycle set-top-box carrier (currently %u kHz)\n",
                getSetTopBoxCarrier());
  Serial.println("Enter 1, 2, 3, 4, 5 or 6:");
}

void printVersion() {
  Serial.printf("\nUniversal IR Remote v%s\n", FIRMWARE_VERSION);
  Serial.println("Board: ESP32-S3");
  Serial.printf("IR receiver: GPIO %u | IR transmitter: GPIO %u\n",
                IR_RECEIVE_PIN, IR_SEND_PIN);
}

void eraseNvsNamespace(const char *name) {
  nvs_handle handle;
  if (nvs_open(name, NVS_READWRITE, &handle) == ESP_OK) {
    nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);
  }
}

void resetNetworkConfiguration() {
  eraseNvsNamespace("WIFI");
  wifiProfilePreferences.clear();
}

void resetHomeKitConfiguration() {
  eraseNvsNamespace("HAP");
  eraseNvsNamespace("CHAR");
}

void resetHomeKitAndWiFi() {
  resetNetworkConfiguration();
  resetHomeKitConfiguration();
  Serial.println("HomeKit pairing and Wi-Fi credentials cleared.");
  Serial.println("IR learning codes were kept. Restarting...");
  delay(500);
  ESP.restart();
}

void listCodes() {
  LearnedCode code;
  Serial.println("\nSaved commands:");
  bool any = false;
  for (uint8_t slot = 0; slot < MAX_CODES; ++slot) {
    if (!loadCode(slot, code)) continue;
    any = true;
    Serial.printf("%2u - %-10s address=0x%X command=0x%X (%u bits)\n", slot + 1,
                  getProtocolString((decode_type_t)code.protocol), code.address,
                  code.command, code.bits);
  }
  if (!any) Serial.println("(No commands learned yet.)");
}

bool parseSlot(const char *text, uint8_t &slot) {
  char *end = nullptr;
  long value = strtol(text, &end, 10);
  if (*text == '\0' || *end != '\0' || value < 1 || value > MAX_CODES) return false;
  slot = (uint8_t)(value - 1);
  return true;
}

void startLearning() {
  consoleMode = LEARN_SELECT_SLOT;
  pendingSlot = -1;
  Serial.printf("\nLearning mode. Enter command number (1-%u), or 0 to exit:\n",
                MAX_CODES);
}

void startSending() {
  consoleMode = SEND_SELECT_SLOT;
  listCodes();
  Serial.printf("Enter command number to send (1-%u), or 0 to exit:\n", MAX_CODES);
}

void saveCode(uint8_t slot, const IRData &signal) {
  LearnedCode code = {};
  code.magic = CODE_MAGIC;
  code.protocol = (uint8_t)signal.protocol;
  code.address = signal.address;
  code.command = signal.command;
  code.extra = signal.extra;
  code.rawCode = signal.decodedRawData;
  code.bits = signal.numberOfBits;
  char key[12];
  recordKey(slot, key, sizeof(key));
  if (preferences.putBytes(key, &code, sizeof(code)) != sizeof(code)) {
    Serial.println("Could not save this command.");
    return;
  }
  Serial.printf("Saved command %u: %s address=0x%X command=0x%X\n", slot + 1,
                getProtocolString(signal.protocol), code.address, code.command);
}

bool transmitCode(uint8_t slot, bool printMessage = true) {
  LearnedCode code;
  if (!loadCode(slot, code)) {
    if (printMessage) Serial.printf("Command %u has not been learned yet.\n", slot + 1);
    return false;
  }
  IRData signal = {};
  signal.protocol = (decode_type_t)code.protocol;
  signal.address = code.address;
  signal.command = code.command;
  signal.extra = code.extra;
  signal.decodedRawData = code.rawCode;
  signal.numberOfBits = code.bits;
  if (printMessage) {
    Serial.printf("Sending command %u: %s address=0x%X command=0x%X\n", slot + 1,
                  getProtocolString(signal.protocol), signal.address, signal.command);
  }
  waitForIrTransmitter();
  IrSender.write(&signal, NO_REPEATS);
  finishIrTransmission();
  return true;
}

void handleIdleCommand(const char *command) {
  if (strcmp(command, "1") == 0) {
    startLearning();
  } else if (strcmp(command, "2") == 0) {
    startSending();
  } else if (strcmp(command, "3") == 0) {
    printVersion();
    printMainMenu();
  } else if (strcmp(command, "4") == 0) {
    resetHomeKitAndWiFi();
  } else if (strcmp(command, "5") == 0) {
    irCaptureVerbose = !irCaptureVerbose;
    Serial.printf("Full IR timing output is now %s.\n",
                  irCaptureVerbose ? "ON" : "OFF");
    printMainMenu();
  } else if (strcmp(command, "6") == 0) {
    Serial.printf("Set-top-box carrier is now %u kHz.\n",
                  cycleSetTopBoxCarrier());
    printMainMenu();
  } else {
    Serial.println("Please enter 1, 2, 3, 4, 5 or 6.");
  }
}

void handleCommand(char *command) {
  if (consoleMode == IDLE) {
    handleIdleCommand(command);
    return;
  }
  if (strcmp(command, "0") == 0) {
    consoleMode = IDLE;
    pendingSlot = -1;
    printMainMenu();
    return;
  }
  uint8_t slot;
  if (!parseSlot(command, slot)) {
    Serial.printf("Enter a number from 1 to %u, or 0 to exit.\n", MAX_CODES);
    return;
  }
  if (consoleMode == LEARN_SELECT_SLOT) {
    pendingSlot = slot;
    consoleMode = LEARN_WAIT_IR;
    Serial.printf("Ready to learn command %u. Press the matching remote button once.\n",
                  slot + 1);
  } else if (consoleMode == SEND_SELECT_SLOT) {
    transmitCode(slot);
    Serial.println("Enter another command number, or 0 to exit:");
  } else {
    Serial.println("Waiting for an infrared command. Press the remote button.");
  }
}

void readSerialCommands() {
  while (Serial.available() > 0) {
    char character = (char)Serial.read();
    if (character == '\r') continue;
    if (character == '\n') {
      serialLine[serialLineLength] = '\0';
      if (serialLineLength > 0) handleCommand(serialLine);
      serialLineLength = 0;
    } else if (serialLineLength < sizeof(serialLine) - 1) {
      serialLine[serialLineLength++] = character;
    } else {
      serialLineLength = 0;
      Serial.println("Input is too long.");
    }
  }
}

void printReceivedCode(const IRData &signal) {
  Serial.println("\n--- IR command received ---");
  Serial.printf("Protocol: %s\n", IrReceiver.getProtocolString());
  Serial.printf("Address : 0x%X\n", signal.address);
  Serial.printf("Command : 0x%X\n", signal.command);
  Serial.printf("Bits    : %u\n", signal.numberOfBits);
  if (signal.flags & IRDATA_FLAGS_IS_REPEAT) Serial.println("Repeat  : yes");

  if (signal.protocol == UNKNOWN || signal.protocol == PULSE_DISTANCE ||
      signal.protocol == PULSE_WIDTH) {
    if (irCaptureVerbose) {
      Serial.println("Raw timing data:");
      IrReceiver.printIRResultRawFormatted(&Serial, true);
    } else if (signal.protocol == UNKNOWN) {
      Serial.println("Raw replay data omitted. Enter 5 at the main menu to show it.");
    } else {
      Serial.println("Compact replay data:");
      IrReceiver.printIRSendUsage(&Serial);
    }
  }
}

void processIR() {
  if (!IrReceiver.decode()) return;
  const auto &signal = IrReceiver.decodedIRData;
  if (signal.protocol == UNKNOWN && signal.rawlen < 20) {
    IrReceiver.resume();
    return;
  }
  printReceivedCode(signal);
  if (isWebLearning() && !(signal.flags & IRDATA_FLAGS_IS_REPEAT)) {
    if (isSupportedForStorage(signal)) {
      saveWebLearnedCode(signal);
    } else {
      Serial.println("Web learning: unsupported raw protocol.");
    }
  } else if (consoleMode == LEARN_WAIT_IR &&
             !(signal.flags & IRDATA_FLAGS_IS_REPEAT)) {
    if (isSupportedForStorage(signal)) {
      saveCode((uint8_t)pendingSlot, signal);
      consoleMode = LEARN_SELECT_SLOT;
      pendingSlot = -1;
      Serial.printf("Enter next command number (1-%u), or 0 to exit:\n", MAX_CODES);
    } else {
      Serial.println("Unsupported raw protocol. Try another command or press 0 to exit.");
    }
  }
  IrReceiver.resume();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("=== Universal IR Remote booting ===");
  Serial.printf("Firmware: %s (%s)\n", FIRMWARE_VERSION, FIRMWARE_BUILD_DATE);
  Serial.printf("Chip: %s, CPU: %u MHz, free heap: %u bytes\n",
                ESP.getChipModel(), ESP.getCpuFreqMHz(), ESP.getFreeHeap());
  Serial.flush();
  loadWiFiProfiles();
  migrateExistingHomeSpanWiFi();
  selectReachableWiFi();
  IrSender.begin(IR_SEND_PIN, DISABLE_LED_FEEDBACK);
  IrReceiver.begin(IR_RECEIVE_PIN, DISABLE_LED_FEEDBACK);
  preferences.begin("ir-codes", false);
  loadBuiltInDeviceConfig();
  if (preferences.getUInt("hapSchema", 0) < 13) {
    homeSpan.forceNewConfigNumber();
    preferences.putUInt("hapSchema", 13);
  }
  homeSpan.setSerialInputDisable(true);
  homeSpan.setApSSID(AP_SSID);
  homeSpan.setApPassword(AP_PASSWORD);
  homeSpan.setApTimeout(AP_SETUP_TIMEOUT_SECONDS);
  homeSpan.setPortNum(HOMEKIT_PORT);
  homeSpan.setPairingCode(HOMEKIT_PAIRING_CODE);
  homeSpan.setSketchVersion(FIRMWARE_VERSION);
  homeSpan.setHostNameSuffix("");
  homeSpan.setConnectionCallback(announceNetworkConnection);
  homeSpan.begin(Category::Bridges, "红外 Hub", "ir-hub");

  new SpanAccessory(HUB_AID);
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("红外 Hub");
      new Characteristic::Manufacturer("ESP32 IR Hub");
      new Characteristic::Model("ESP32-S3 IR Bridge");
      new Characteristic::FirmwareRevision(FIRMWARE_VERSION);

  if (isBuiltInDeviceEnabled(0)) configureAirConditionerAccessory();
  if (isBuiltInDeviceEnabled(1)) configureTelevisionAccessory();
  configureUserDevices();
  beginDeviceManager();
  startSetupHotspot();
  printMainMenu();
}

void loop() {
  homeSpan.poll();
  pollSetupHotspot();
  pollDeviceManager();
  readSerialCommands();
  processIR();
}
