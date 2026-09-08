#pragma once

constexpr uint8_t MAX_USER_DEVICES = 8;
constexpr uint8_t MAX_DEVICE_ACTIONS = 15;
constexpr uint32_t DEVICE_MAGIC = 0x49524432;  // "IRD2"
constexpr uint16_t DEVICE_SCHEMA_VERSION = 1;

// Keep the first four values stable because they are stored in NVS.
enum UserDeviceType : uint8_t {
  USER_SWITCH, USER_FAN, USER_TELEVISION, USER_AIR_CONDITIONER,
  USER_LIGHT_BULB, USER_OUTLET, USER_THERMOSTAT
};
enum UserAction : uint8_t {
  ACTION_POWER, ACTION_SPEED_UP, ACTION_SPEED_DOWN, ACTION_VOLUME_UP,
  ACTION_VOLUME_DOWN, ACTION_CHANNEL_UP, ACTION_CHANNEL_DOWN, ACTION_UP,
  ACTION_DOWN, ACTION_LEFT, ACTION_RIGHT, ACTION_SELECT, ACTION_TEMPERATURE_UP,
  ACTION_TEMPERATURE_DOWN, ACTION_MODE
};

struct __attribute__((packed)) UserDevice {
  uint32_t magic;
  uint16_t schema;
  uint8_t type;
  uint8_t enabled;
  char name[32];
};

void loadBuiltInDeviceConfig();
bool isBuiltInDeviceEnabled(uint8_t device);
const char *builtInDeviceName(uint8_t device);
uint8_t builtInSetTopBoxCarrier();
void setBuiltInSetTopBoxCarrier(uint8_t carrier);
