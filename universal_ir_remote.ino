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

constexpr uint8_t IR_RECEIVE_PIN = 41;
constexpr uint8_t IR_SEND_PIN = 40;
constexpr uint8_t MAX_CODES = 20;
constexpr uint32_t CODE_MAGIC = 0x49524332;
constexpr char FIRMWARE_VERSION[] = "2.01";
constexpr char FIRMWARE_BUILD_DATE[] = __DATE__ " " __TIME__;
constexpr char AP_SSID[] = "IR-AC-Setup";
constexpr char AP_PASSWORD[] = "iracsetup";
constexpr char HOMEKIT_PAIRING_CODE[] = "11122333";

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
ConsoleMode consoleMode = IDLE;
int8_t pendingSlot = -1;
char serialLine[32] = {};
uint8_t serialLineLength = 0;
bool irCaptureVerbose = IR_CAPTURE_VERBOSE;
uint32_t lastIrTransmissionFinishedAt = 0;

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

void resetNetworkConfiguration() { eraseNvsNamespace("WIFI"); }

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
  delay(500);
  IrSender.begin(IR_SEND_PIN, DISABLE_LED_FEEDBACK);
  IrReceiver.begin(IR_RECEIVE_PIN, DISABLE_LED_FEEDBACK);
  preferences.begin("ir-codes", false);
  loadBuiltInDeviceConfig();
  if (preferences.getUInt("hapSchema", 0) < 10) {
    homeSpan.forceNewConfigNumber();
    preferences.putUInt("hapSchema", 10);
  }
  homeSpan.setSerialInputDisable(true);
  homeSpan.setApSSID(AP_SSID);
  homeSpan.setApPassword(AP_PASSWORD);
  homeSpan.setApTimeout(600);
  homeSpan.enableAutoStartAP();
  homeSpan.setPairingCode(HOMEKIT_PAIRING_CODE);
  homeSpan.setSketchVersion(FIRMWARE_VERSION);
  homeSpan.setHostNameSuffix("");
  homeSpan.setConnectionCallback(announceNetworkConnection);
  homeSpan.begin(Category::Bridges, "红外 Hub", "ir-hub");

  new SpanAccessory();
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
  printMainMenu();
}

void loop() {
  homeSpan.poll();
  pollDeviceManager();
  readSerialCommands();
  processIR();
}
