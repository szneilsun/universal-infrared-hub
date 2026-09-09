// Meiling KKZM-4WB protocol and HomeKit air-conditioner accessory.

enum MeilingMode : uint8_t { MEILING_HEAT, MEILING_COOL };
enum MeilingAction : uint8_t {
  MEILING_STATE, MEILING_MODE, MEILING_POWER_ON, MEILING_POWER_OFF,
  MEILING_DISPLAY_TOGGLE, MEILING_FAN_SPEED
};

constexpr char MEILING_FAN_LEVEL_KEY[] = "ac-fan";

uint8_t fanLevelForSpeed(float speed) {
  // Home App submits an arbitrary percentage while dragging.  The Meiling
  // remote only has five real levels, so always select the nearest level.
  return constrain((uint8_t)(speed / 20.0f + 0.5f), 1, 5);
}

uint8_t loadMeilingFanLevel() {
  return constrain(preferences.getUChar(MEILING_FAN_LEVEL_KEY, 1), 1, 5);
}

void saveMeilingFanLevel(uint8_t level) {
  preferences.putUChar(MEILING_FAN_LEVEL_KEY, constrain(level, 1, 5));
}

uint8_t meilingChecksum(const uint8_t *data, size_t length) {
  uint8_t sum = 0;
  for (size_t i = 0; i < length; ++i)
    sum += (data[i] >> 4) + (data[i] & 0x0F);
  return sum;
}

bool sendMeiling(uint16_t temperatureTenths, MeilingMode mode,
                 MeilingAction action, uint8_t fanLevel = 0) {
  temperatureTenths = constrain(temperatureTenths, 160, 320);
  const uint8_t wholeDegrees = temperatureTenths / 10;
  const uint8_t tenths = temperatureTenths % 10;
  uint8_t frame[15] = {};
  frame[0] = 0x56;
  frame[1] = 0x5C + wholeDegrees;
  frame[3] = tenths << 4;
  frame[4] = mode == MEILING_HEAT ? 0x10 : 0x22;

  if (action == MEILING_POWER_OFF) frame[5] = 0xC0;
  if (action == MEILING_DISPLAY_TOGGLE) frame[5] = 0x04;
  if (action == MEILING_FAN_SPEED) {
    uint8_t modeBase = mode == MEILING_HEAT ? 0x10 : 0x20;
    switch (constrain(fanLevel, 0, 5)) {
      case 0: frame[4] = modeBase; break;
      case 1: frame[3] |= 0x01; frame[4] = modeBase | 2; break;
      case 2: frame[4] = modeBase | 2; break;
      case 3: frame[4] = modeBase | 3; break;
      case 4: frame[4] = modeBase | 1; break;
      case 5: frame[4] = modeBase | 1; frame[8] = 0x80; break;
    }
  }
  frame[14] = meilingChecksum(frame, 14);

  uint64_t rawData[2] = {};
  for (uint8_t i = 0; i < 8; ++i) rawData[0] |= (uint64_t)frame[i] << (i * 8);
  for (uint8_t i = 0; i < 7; ++i) rawData[1] |= (uint64_t)frame[i + 8] << (i * 8);

  const char *actionName = action == MEILING_POWER_ON ? "power-on"
      : action == MEILING_POWER_OFF ? "power-off"
      : action == MEILING_MODE ? "mode"
      : action == MEILING_DISPLAY_TOGGLE ? "display-toggle"
      : action == MEILING_FAN_SPEED ? "fan-speed" : "temperature";
  if (action == MEILING_FAN_SPEED) {
    Serial.printf("Meiling: action=%s, mode=%s, temperature=%u.%u C, "
                  "fan-level=%u/5, checksum=0x%02X\n",
                  actionName, mode == MEILING_HEAT ? "heat" : "cool",
                  wholeDegrees, tenths, constrain(fanLevel, 1, 5), frame[14]);
  } else {
    Serial.printf("Meiling: action=%s, mode=%s, temperature=%u.%u C, checksum=0x%02X\n",
                  actionName, mode == MEILING_HEAT ? "heat" : "cool",
                  wholeDegrees, tenths, frame[14]);
  }
  waitForIrTransmitter();
  IrSender.sendPulseDistanceWidthFromArray(
      38, 8450, 4200, 550, 1600, 550, 550, rawData, 120,
      PROTOCOL_IS_LSB_FIRST, 0, 0);
  finishIrTransmission();
  return true;
}

struct IRAirConditioner : Service::HeaterCooler {
  SpanCharacteristic *active;
  SpanCharacteristic *currentTemperature;
  SpanCharacteristic *currentState;
  SpanCharacteristic *targetState;
  SpanCharacteristic *coolingThreshold;
  SpanCharacteristic *heatingThreshold;
  SpanCharacteristic *rotationSpeed;
  SpanCharacteristic *displayToggle;
  bool restoreActiveAfterFanChange = false;
  bool syncFanSpeedAfterUpdate = false;
  uint32_t zeroSpeedCommandAt = 0;
  uint8_t fanLevel;

  IRAirConditioner() : Service::HeaterCooler() {
    active = new Characteristic::Active(0);
    currentTemperature = new Characteristic::CurrentTemperature(24);
    currentState = new Characteristic::CurrentHeaterCoolerState(0);
    targetState = new Characteristic::TargetHeaterCoolerState(2);
    targetState->setValidValues(2, 1, 2);
    coolingThreshold = new Characteristic::CoolingThresholdTemperature(24);
    coolingThreshold->setRange(16, 32, 0.1);
    heatingThreshold = new Characteristic::HeatingThresholdTemperature(20);
    heatingThreshold->setRange(16, 32, 0.1);
    new Characteristic::TemperatureDisplayUnits(0);
    fanLevel = loadMeilingFanLevel();
    rotationSpeed = new Characteristic::RotationSpeed(fanLevel * 20);
    rotationSpeed->setRange(20, 100, 20);
    displayToggle = new Characteristic::SwingMode(0);
    new Characteristic::ConfiguredName(builtInDeviceName(0));
  }

  boolean update() override {
    bool activeChanged = active->updated() &&
                         active->getNewVal() != active->getVal();

    if (!active->getVal() && active->updated() && active->getNewVal() &&
        (rotationSpeed->updated() || displayToggle->updated())) {
      Serial.println("Ignored fan/display change while air conditioner is off.");
      return false;
    }

    uint8_t requestedMode = targetState->updated() ? targetState->getNewVal()
                                                   : targetState->getVal();
    MeilingMode mode = requestedMode == 1 ? MEILING_HEAT : MEILING_COOL;
    // Siri may write either threshold characteristic regardless of the current
    // mode. Always prefer the characteristic changed by this HomeKit request.
    float requestedTemperature;
    if (coolingThreshold->updated()) {
      requestedTemperature = coolingThreshold->getNewVal<float>();
    } else if (heatingThreshold->updated()) {
      requestedTemperature = heatingThreshold->getNewVal<float>();
    } else {
      requestedTemperature = mode == MEILING_HEAT
          ? heatingThreshold->getVal<float>()
          : coolingThreshold->getVal<float>();
    }
    uint16_t temperatureTenths =
        (uint16_t)(requestedTemperature * 10.0f + 0.5f);

    if (rotationSpeed->updated() && active->updated() && !active->getNewVal()) {
      float requestedSpeed = rotationSpeed->getNewVal<float>();
      fanLevel = fanLevelForSpeed(requestedSpeed);
      if (!sendMeiling(temperatureTenths, mode, MEILING_FAN_SPEED, fanLevel))
        return false;
      syncFanSpeedAfterUpdate = true;
      saveMeilingFanLevel(fanLevel);
      if (fanLevel == 1) zeroSpeedCommandAt = millis();
      currentState->setVal(mode == MEILING_HEAT ? 2 : 3);
      currentTemperature->setVal(requestedTemperature);
      restoreActiveAfterFanChange = true;
      return true;
    }

    if (activeChanged && !active->getNewVal() && zeroSpeedCommandAt != 0 &&
        millis() - zeroSpeedCommandAt < 3000) {
      Serial.println("HomeKit minimum fan speed: keeping air conditioner active.");
      restoreActiveAfterFanChange = true;
      return true;
    }

    if (coolingThreshold->updated() || heatingThreshold->updated()) {
      // Keep both HomeKit target-temperature fields aligned so Home/Siri show
      // one target temperature when the operating mode changes.
      if (!coolingThreshold->updated())
        coolingThreshold->setVal(requestedTemperature);
      if (!heatingThreshold->updated())
        heatingThreshold->setVal(requestedTemperature);
    }

    if (activeChanged) {
      bool turnOn = active->getNewVal();
      if (!sendMeiling(temperatureTenths, mode,
                       turnOn ? MEILING_POWER_ON : MEILING_POWER_OFF)) return false;
      if (!turnOn) {
        currentState->setVal(0);
      } else {
        currentState->setVal(mode == MEILING_HEAT ? 2 : 3);
        currentTemperature->setVal(requestedTemperature);
      }
    } else if (targetState->updated()) {
      if (active->getVal()) {
        if (!sendMeiling(temperatureTenths, mode, MEILING_MODE)) return false;
        currentState->setVal(mode == MEILING_HEAT ? 2 : 3);
        currentTemperature->setVal(requestedTemperature);
      }
    } else if (coolingThreshold->updated() || heatingThreshold->updated()) {
      if (active->getVal() &&
          !sendMeiling(temperatureTenths, mode, MEILING_STATE)) return false;
      currentTemperature->setVal(requestedTemperature);
    } else if (rotationSpeed->updated()) {
      float requestedSpeed = rotationSpeed->getNewVal<float>();
      fanLevel = fanLevelForSpeed(requestedSpeed);
      if (active->getVal() &&
          !sendMeiling(temperatureTenths, mode, MEILING_FAN_SPEED, fanLevel))
        return false;
      syncFanSpeedAfterUpdate = true;
      saveMeilingFanLevel(fanLevel);
      if (fanLevel == 1) zeroSpeedCommandAt = millis();
    } else if (displayToggle->updated()) {
      if (active->getVal() &&
          !sendMeiling(temperatureTenths, mode, MEILING_DISPLAY_TOGGLE))
        return false;
    }
    return true;
  }

  void loop() override {
    if (syncFanSpeedAfterUpdate) {
      float snappedSpeed = fanLevel * 20;
      if (rotationSpeed->getVal<float>() != snappedSpeed)
        rotationSpeed->setVal(snappedSpeed);
      syncFanSpeedAfterUpdate = false;
    }
    if (restoreActiveAfterFanChange) {
      if (!active->getVal()) active->setVal(1);
      restoreActiveAfterFanChange = false;
    }
  }
};

void configureAirConditionerAccessory() {
  new SpanAccessory(AIR_CONDITIONER_AID);
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name(builtInDeviceName(0));
      new Characteristic::Manufacturer("ESP32 IR Hub");
      new Characteristic::Model("Meiling KKZM-4WB");
      new Characteristic::FirmwareRevision(FIRMWARE_VERSION);
    new IRAirConditioner();
}

void sendWebAirConditionerTest(uint8_t action) {
  if (action == 0) sendMeiling(240, MEILING_COOL, MEILING_POWER_ON);
  else if (action == 1) sendMeiling(240, MEILING_COOL, MEILING_POWER_OFF);
  else if (action == 2) sendMeiling(240, MEILING_COOL, MEILING_DISPLAY_TOGGLE);
}
