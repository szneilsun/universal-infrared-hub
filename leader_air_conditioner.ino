// Leader air-conditioner protocol reconstructed from the supplied remote frames.

enum LeaderAction : uint8_t { LEADER_POWER, LEADER_TEMPERATURE, LEADER_FAN };

constexpr char LEADER_FAN_LEVEL_KEY[] = "leader-fan";
constexpr uint8_t LEADER_FAN_LEVELS = 8;

const uint8_t LEADER_FAN_B5[LEADER_FAN_LEVELS] = {
    0x05, 0x05, 0x06, 0x02, 0x02, 0x02, 0x04, 0x05};
const uint8_t LEADER_FAN_B6[LEADER_FAN_LEVELS] = {
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
const uint8_t LEADER_FAN_AUX[LEADER_FAN_LEVELS] = {
    0x00, 0x00, 0x03, 0x05, 0x01, 0x06, 0x02, 0x00};
uint8_t reverseLeaderNibble(uint8_t value) {
  value &= 0x0F;
  return ((value & 0x01) << 3) | ((value & 0x02) << 1) |
         ((value & 0x04) >> 1) | ((value & 0x08) >> 3);
}

uint8_t reverseLeaderByte(uint8_t value) {
  value = (value >> 4) | (value << 4);
  value = ((value & 0xCC) >> 2) | ((value & 0x33) << 2);
  return ((value & 0xAA) >> 1) | ((value & 0x55) << 1);
}

uint8_t leaderChecksum(const uint8_t *frame, size_t start, size_t end) {
  uint8_t sum = 0;
  for (size_t i = start; i < end; ++i) sum += reverseLeaderByte(frame[i]);
  return reverseLeaderByte(sum);
}

uint8_t leaderFanLevelForSpeed(float speed) {
  return constrain((uint8_t)(speed / 12.5f + 0.5f), 1, LEADER_FAN_LEVELS);
}

uint8_t loadLeaderFanLevel() {
  return constrain(preferences.getUChar(LEADER_FAN_LEVEL_KEY, 1), 1,
                   LEADER_FAN_LEVELS);
}

void saveLeaderFanLevel(uint8_t level) {
  preferences.putUChar(LEADER_FAN_LEVEL_KEY,
                       constrain(level, 1, LEADER_FAN_LEVELS));
}

bool sendLeader(uint16_t temperatureTenths, uint8_t fanLevel,
                LeaderAction action, bool powerOn = true) {
  temperatureTenths = constrain(temperatureTenths, 160, 300);
  fanLevel = constrain(fanLevel, 1, LEADER_FAN_LEVELS);
  const uint8_t wholeDegrees = temperatureTenths / 10;
  const bool halfDegree = temperatureTenths % 10 >= 5;
  const uint8_t fanIndex = fanLevel - 1;
  uint8_t frame[22] = {};

  frame[0] = 0x65;
  frame[1] = 0x40 | reverseLeaderNibble(wholeDegrees - 16);
  frame[2] = 0x03;
  frame[4] = powerOn ? 0x02 : 0x00;
  frame[5] = LEADER_FAN_B5[fanIndex];
  frame[6] = LEADER_FAN_B6[fanIndex];
  frame[7] = 0x04;
  frame[10] = halfDegree ? 0x80 : 0x00;
  // B12 records the remote operation: power, fan change, or state update.
  frame[12] = action == LEADER_POWER ? 0xA0 :
              action == LEADER_FAN ? 0x20 : 0x00;
  frame[13] = leaderChecksum(frame, 0, 13);
  frame[14] = 0xED;
  frame[16] = LEADER_FAN_AUX[fanIndex];
  frame[21] = leaderChecksum(frame, 14, 21);

  uint16_t timings[357] = {};
  size_t index = 0;
  timings[index++] = 3100;
  timings[index++] = 3100;
  timings[index++] = 3050;
  timings[index++] = 4600;
  for (uint8_t byte : frame) {
    for (uint8_t bit = 0; bit < 8; ++bit) {
      timings[index++] = 550;
      timings[index++] = byte & (1U << bit) ? 1700 : 600;
    }
  }
  timings[index++] = 550;

  waitForIrTransmitter();
  const bool sent = sendHardwareIrTimings(timings, index, 38);
  finishIrTransmission();
  Serial.printf("Leader: action=%u, power=%s, temperature=%u.%u C, fan=%u/8\n",
                action, powerOn ? "on" : "off", wholeDegrees,
                halfDegree ? 5 : 0, fanLevel);
  return sent;
}

struct IRLeaderAirConditioner : Service::HeaterCooler {
  SpanCharacteristic *active;
  SpanCharacteristic *currentTemperature;
  SpanCharacteristic *currentState;
  SpanCharacteristic *targetState;
  SpanCharacteristic *coolingThreshold;
  SpanCharacteristic *heatingThreshold;
  SpanCharacteristic *rotationSpeed;
  uint8_t fanLevel;
  bool syncFanSpeed = false;

  IRLeaderAirConditioner() : Service::HeaterCooler() {
    active = new Characteristic::Active(0);
    currentTemperature = new Characteristic::CurrentTemperature(24);
    currentState = new Characteristic::CurrentHeaterCoolerState(0);
    targetState = new Characteristic::TargetHeaterCoolerState(2);
    targetState->setValidValues(1, 2);  // Captured frame set currently covers cooling.
    coolingThreshold = new Characteristic::CoolingThresholdTemperature(24);
    coolingThreshold->setRange(16, 30, 0.5);
    heatingThreshold = new Characteristic::HeatingThresholdTemperature(24);
    heatingThreshold->setRange(16, 30, 0.5);
    new Characteristic::TemperatureDisplayUnits(0);
    fanLevel = loadLeaderFanLevel();
    rotationSpeed = new Characteristic::RotationSpeed(fanLevel * 12.5f);
    rotationSpeed->setRange(12.5, 100, 12.5);
    new Characteristic::ConfiguredName(builtInDeviceName(BUILTIN_LEADER_AIR_CONDITIONER));
  }

  boolean update() override {
    float requestedTemperature = coolingThreshold->updated()
        ? coolingThreshold->getNewVal<float>()
        : heatingThreshold->updated() ? heatingThreshold->getNewVal<float>()
        : coolingThreshold->getVal<float>();
    uint16_t temperatureTenths = (uint16_t)(requestedTemperature * 10.0f + 0.5f);

    if (coolingThreshold->updated() || heatingThreshold->updated()) {
      coolingThreshold->setVal(requestedTemperature);
      heatingThreshold->setVal(requestedTemperature);
    }
    if (active->updated() && active->getNewVal() != active->getVal()) {
      bool turnOn = active->getNewVal();
      if (!sendLeader(temperatureTenths, fanLevel, LEADER_POWER, turnOn)) return false;
      currentState->setVal(turnOn ? 3 : 0);
      if (turnOn) currentTemperature->setVal(requestedTemperature);
      return true;
    }
    if (rotationSpeed->updated()) {
      fanLevel = leaderFanLevelForSpeed(rotationSpeed->getNewVal<float>());
      if (active->getVal() &&
          !sendLeader(temperatureTenths, fanLevel, LEADER_FAN)) return false;
      saveLeaderFanLevel(fanLevel);
      syncFanSpeed = true;
      return true;
    }
    if ((coolingThreshold->updated() || heatingThreshold->updated()) && active->getVal()) {
      if (!sendLeader(temperatureTenths, fanLevel, LEADER_TEMPERATURE)) return false;
      currentTemperature->setVal(requestedTemperature);
    }
    return true;
  }

  void loop() override {
    if (!syncFanSpeed) return;
    rotationSpeed->setVal(fanLevel * 12.5f);
    syncFanSpeed = false;
  }
};

void configureLeaderAirConditionerAccessory() {
  new SpanAccessory(LEADER_AIR_CONDITIONER_AID);
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name(builtInDeviceName(BUILTIN_LEADER_AIR_CONDITIONER));
      new Characteristic::Manufacturer("Leader");
      new Characteristic::Model("Leader IR 176-bit");
      new Characteristic::FirmwareRevision(FIRMWARE_VERSION);
    new IRLeaderAirConditioner();
}

void sendWebLeaderAirConditionerTest(uint8_t action) {
  sendLeader(240, 1, LEADER_POWER, action == 0);
}
