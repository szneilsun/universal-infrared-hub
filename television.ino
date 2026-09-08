// Pioneer television/set-top-box protocols and HomeKit television accessory.

enum TelevisionCommand : uint8_t {
  TV_POWER, TV_VOLUME_UP, TV_VOLUME_DOWN, TV_CHANNEL_UP, TV_CHANNEL_DOWN,
  TV_SELECT, SET_TOP_BOX_POWER
};

uint32_t lastTelevisionControlAt = 0;
constexpr uint8_t SET_TOP_BOX_CARRIERS_KHZ[] = {38, 40, 36, 56};
uint8_t setTopBoxCarrierIndex = 0;

uint8_t getSetTopBoxCarrier() {
  return SET_TOP_BOX_CARRIERS_KHZ[setTopBoxCarrierIndex];
}

void setSetTopBoxCarrier(uint8_t carrier) {
  for (uint8_t i = 0; i < sizeof(SET_TOP_BOX_CARRIERS_KHZ) / sizeof(SET_TOP_BOX_CARRIERS_KHZ[0]); ++i) {
    if (SET_TOP_BOX_CARRIERS_KHZ[i] == carrier) {
      setTopBoxCarrierIndex = i;
      return;
    }
  }
}

uint8_t cycleSetTopBoxCarrier() {
  setTopBoxCarrierIndex =
      (setTopBoxCarrierIndex + 1) %
      (sizeof(SET_TOP_BOX_CARRIERS_KHZ) / sizeof(SET_TOP_BOX_CARRIERS_KHZ[0]));
  setBuiltInSetTopBoxCarrier(getSetTopBoxCarrier());
  return getSetTopBoxCarrier();
}

void sendSetTopBoxPower() {
  static const uint16_t raw[] = {
      3650, 1800,
      600, 1100, 550, 1150, 550, 1100, 550, 1150,
      550, 600, 550, 550, 600, 1100, 550, 600,
      550, 1150, 550, 1100, 600, 550, 600, 550,
      600, 550, 550, 600, 600, 1100, 550, 600,
      550, 600, 550, 1100, 550, 1150, 550, 1150,
      550, 550, 600, 550, 600, 1100, 550, 600,
      3600, 1800,
      550, 1150, 550, 1100, 600, 1100, 550, 1100,
      600, 550, 600, 550, 550, 600, 550, 600,
      550, 600, 550, 550, 600, 550, 600, 550,
      600, 1100, 550, 1150, 550, 1100, 550, 1150,
      550};
  waitForIrTransmitter();
  Serial.printf("Television: action=set-top-box-power, raw %u kHz\n",
                getSetTopBoxCarrier());
  IrSender.sendRaw(raw, sizeof(raw) / sizeof(raw[0]), getSetTopBoxCarrier());
  finishIrTransmission();
}

void sendSetTopBoxRemote(uint16_t command, const char *name) {
  uint16_t raw[85];
  uint8_t index = 0;
  auto append = [&](uint16_t duration) { raw[index++] = duration; };
  auto appendBits = [&](uint32_t data, uint8_t bitCount) {
    for (int8_t bit = bitCount - 1; bit >= 0; --bit) {
      append(550);
      // Replay the working remote timing directly. A demodulating receiver
      // introduces mark/space distortion, so loopback must not be compensated.
      append(data & (1UL << bit) ? 1100 : 550);
    }
  };
  append(3650);
  append(1800);
  appendBits(0xF2C272, 24);
  append(3650);
  append(1800);
  appendBits(command, 16);
  append(550);

  waitForIrTransmitter();
  Serial.printf("Pioneer set-top box: action=%s, raw command=0x%04X\n",
                name, command);
  IrSender.sendRaw(raw, index, getSetTopBoxCarrier());
  finishIrTransmission();
  lastTelevisionControlAt = millis();
}

void sendTelevisionCommand(uint8_t command) {
  uint16_t address = 0;
  uint8_t value = 0;
  const char *name = "unknown";
  switch (command) {
    case TV_POWER: address = 0xAA; value = 0x1C; name = "power"; break;
    case TV_VOLUME_UP: address = 0xAA; value = 0x0A; name = "volume-up"; break;
    case TV_VOLUME_DOWN: address = 0xAA; value = 0x0B; name = "volume-down"; break;
    case TV_CHANNEL_UP: address = 0x00; value = 0xF2; name = "channel-up"; break;
    case TV_CHANNEL_DOWN: address = 0x00; value = 0xF4; name = "channel-down"; break;
    case TV_SELECT: address = 0x00; value = 0xF3; name = "select"; break;
    case SET_TOP_BOX_POWER: sendSetTopBoxPower(); return;
  }
  waitForIrTransmitter();
  Serial.printf("Television: action=%s, NEC address=0x%X command=0x%X\n",
                name, address, value);
  IrSender.sendNEC(address, value, 0);
  finishIrTransmission();
  if (command != TV_POWER) lastTelevisionControlAt = millis();
}

struct IRTelevision : Service::Television {
  SpanCharacteristic *active;
  SpanCharacteristic *remoteKey;

  IRTelevision() : Service::Television() {
    active = new Characteristic::Active(0);
    remoteKey = new Characteristic::RemoteKey();
    new Characteristic::ConfiguredName(builtInDeviceName(1));
  }

  boolean update() override {
    if (active->updated()) {
      if (lastTelevisionControlAt != 0 &&
          millis() - lastTelevisionControlAt < 1500) {
        Serial.println("Ignored HomeKit TV power sync after remote command.");
        return false;
      }
      sendTelevisionCommand(TV_POWER);
    }
    if (remoteKey->updated()) {
      switch (remoteKey->getNewVal()) {
        case 4: sendSetTopBoxRemote(0xD42B, "up"); break;
        case 5: sendSetTopBoxRemote(0xA45B, "down"); break;
        case 6: sendSetTopBoxRemote(0x946B, "left"); break;
        case 7: sendSetTopBoxRemote(0x14EB, "right"); break;
        case 8: sendSetTopBoxRemote(0x34CB, "select"); break;
        default:
          Serial.printf("Pioneer set-top box: unused remote key %u.\n",
                        (unsigned)remoteKey->getNewVal());
          break;
      }
    }
    return true;
  }
};

struct IRTelevisionSpeaker : Service::TelevisionSpeaker {
  SpanCharacteristic *volumeSelector;

  IRTelevisionSpeaker() : Service::TelevisionSpeaker() {
    new Characteristic::VolumeControlType(1);
    volumeSelector = new Characteristic::VolumeSelector();
    new Characteristic::ConfiguredName("Pioneer机顶盒音量");
  }

  boolean update() override {
    if (volumeSelector->updated()) {
      sendTelevisionCommand(volumeSelector->getNewVal() == 0
                                ? TV_VOLUME_UP : TV_VOLUME_DOWN);
    }
    return true;
  }
};

void configureTelevisionAccessory() {
  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name(builtInDeviceName(1));
      new Characteristic::Manufacturer("ESP32 IR Hub");
      new Characteristic::Model("Pioneer Set-top Box");
      new Characteristic::FirmwareRevision(FIRMWARE_VERSION);
    SpanService *televisionSpeaker = new IRTelevisionSpeaker();
    (new IRTelevision())->addLink(televisionSpeaker);
}

void sendWebTelevisionTest(uint8_t action) {
  switch (action) {
    case 0: sendTelevisionCommand(TV_POWER); break;
    case 1: sendTelevisionCommand(TV_VOLUME_UP); break;
    case 2: sendTelevisionCommand(TV_VOLUME_DOWN); break;
  }
}
