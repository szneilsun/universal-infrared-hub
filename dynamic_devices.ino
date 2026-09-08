// Dynamic learned devices, their HomeKit services, and the local setup page.

WebServer deviceServer(8080);
int8_t webLearningDevice = -1;
int8_t webLearningAction = -1;
uint32_t restartAt = 0;

void deviceKey(uint8_t id, char *key, size_t keySize) {
  snprintf(key, keySize, "dev%u", id);
}

void deviceCodeKey(uint8_t id, uint8_t action, char *key, size_t keySize) {
  snprintf(key, keySize, "dev%uk%u", id, action);
}

bool loadUserDevice(uint8_t id, UserDevice &device) {
  char key[12];
  deviceKey(id, key, sizeof(key));
  if (preferences.getBytesLength(key) != sizeof(UserDevice)) return false;
  preferences.getBytes(key, &device, sizeof(device));
  return device.magic == DEVICE_MAGIC && device.schema == DEVICE_SCHEMA_VERSION &&
         device.type <= USER_TELEVISION;
}

bool saveUserDevice(uint8_t id, const UserDevice &device) {
  char key[12];
  deviceKey(id, key, sizeof(key));
  return preferences.putBytes(key, &device, sizeof(device)) == sizeof(device);
}

bool loadUserCode(uint8_t id, uint8_t action, LearnedCode &code) {
  char key[12];
  deviceCodeKey(id, action, key, sizeof(key));
  if (preferences.getBytesLength(key) != sizeof(LearnedCode)) return false;
  preferences.getBytes(key, &code, sizeof(code));
  return code.magic == CODE_MAGIC;
}

bool sendUserCode(uint8_t id, uint8_t action) {
  LearnedCode code;
  if (!loadUserCode(id, action, code)) {
    Serial.printf("Device %u action %u has not been learned.\n", id + 1, action);
    return false;
  }
  IRData signal = {};
  signal.protocol = (decode_type_t)code.protocol;
  signal.address = code.address;
  signal.command = code.command;
  signal.extra = code.extra;
  signal.decodedRawData = code.rawCode;
  signal.numberOfBits = code.bits;
  Serial.printf("Learned device %u: sending action %u.\n", id + 1, action);
  waitForIrTransmitter();
  IrSender.write(&signal, NO_REPEATS);
  finishIrTransmission();
  return true;
}

const char *deviceTypeName(uint8_t type) {
  switch (type) {
    case USER_SWITCH: return "开关";
    case USER_FAN: return "风扇";
    case USER_TELEVISION: return "电视";
    default: return "未知";
  }
}

const char *actionName(uint8_t action) {
  switch (action) {
    case ACTION_POWER: return "电源 / 开关";
    case ACTION_SPEED_UP: return "风速增加";
    case ACTION_SPEED_DOWN: return "风速降低";
    case ACTION_VOLUME_UP: return "音量增加";
    case ACTION_VOLUME_DOWN: return "音量降低";
    case ACTION_CHANNEL_UP: return "频道增加";
    case ACTION_CHANNEL_DOWN: return "频道降低";
    case ACTION_UP: return "上";
    case ACTION_DOWN: return "下";
    case ACTION_LEFT: return "左";
    case ACTION_RIGHT: return "右";
    case ACTION_SELECT: return "确认";
    default: return "未定义";
  }
}

uint8_t actionsForType(uint8_t type, const uint8_t *&actions) {
  static const uint8_t switchActions[] = {ACTION_POWER};
  static const uint8_t fanActions[] = {ACTION_POWER, ACTION_SPEED_UP, ACTION_SPEED_DOWN};
  static const uint8_t televisionActions[] = {
      ACTION_POWER, ACTION_VOLUME_UP, ACTION_VOLUME_DOWN, ACTION_CHANNEL_UP,
      ACTION_CHANNEL_DOWN, ACTION_UP, ACTION_DOWN, ACTION_LEFT, ACTION_RIGHT,
      ACTION_SELECT};
  if (type == USER_SWITCH) { actions = switchActions; return 1; }
  if (type == USER_FAN) { actions = fanActions; return 3; }
  actions = televisionActions;
  return 10;
}

String htmlEscape(const char *text) {
  String escaped;
  while (*text) {
    if (*text == '&') escaped += "&amp;";
    else if (*text == '<') escaped += "&lt;";
    else if (*text == '>') escaped += "&gt;";
    else if (*text == '\"') escaped += "&quot;";
    else escaped += *text;
    ++text;
  }
  return escaped;
}

String pageHeader() {
  return F("<!doctype html><html lang='zh-CN'><meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>红外 Hub 设备管理</title><style>body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;max-width:720px;margin:24px auto;padding:0 16px;color:#18212b}"
           "section{border:1px solid #d7dce1;border-radius:12px;padding:16px;margin:14px 0}button{background:#0673e6;color:#fff;border:0;border-radius:7px;padding:9px 12px;margin:4px 3px 4px 0;font-size:14px}"
           "input,select{font-size:16px;padding:8px;margin:4px;max-width:100%}.muted{color:#617080;font-size:14px}.ok{color:#087443}</style><body><h1>红外 Hub 设备管理</h1>");
}

void sendManagerPage() {
  String html = pageHeader();
  html += F("<p class='muted'>在此新增遥控器、命名并逐键学习。完成后 Hub 将自动重启，新的设备会显示在 Apple 家庭 App。</p>");
  for (uint8_t id = 0; id < MAX_USER_DEVICES; ++id) {
    UserDevice device;
    if (!loadUserDevice(id, device)) continue;
    html += "<section><h2>" + htmlEscape(device.name) + " <small>（" + deviceTypeName(device.type) + "）</small></h2>";
    html += device.enabled ? "<p class='ok'>已发布到 HomeKit</p>" : "<p class='muted'>配置中，尚未发布</p>";
    const uint8_t *actions;
    uint8_t count = actionsForType(device.type, actions);
    for (uint8_t i = 0; i < count; ++i) {
      LearnedCode code;
      bool learned = loadUserCode(id, actions[i], code);
      html += "<form method='post' action='/learn' style='display:inline'><input type='hidden' name='id' value='" + String(id) + "'><input type='hidden' name='action' value='" + String(actions[i]) + "'><button>";
      html += learned ? "重新学习：" : "学习：";
      html += actionName(actions[i]);
      html += "</button></form>";
    }
    if (!device.enabled) {
      html += "<form method='post' action='/publish'><input type='hidden' name='id' value='" + String(id) + "'><button>完成并发布此设备</button></form>";
    }
    html += "</section>";
  }
  html += F("<section><h2>新建设备</h2><form method='post' action='/create'><input name='name' maxlength='31' required placeholder='例如：客厅电视'>"
            "<select name='type'><option value='0'>普通开关</option><option value='1'>风扇</option><option value='2'>电视</option></select><button>创建并开始学习</button></form>"
            "<p class='muted'>提示：带独立开/关按键的设备，目前请学习常用的“开关”键；状态无法由红外反向读取。</p></section></body></html>");
  deviceServer.send(200, "text/html; charset=utf-8", html);
}

void handleCreateDevice() {
  if (!deviceServer.hasArg("name") || !deviceServer.hasArg("type")) {
    deviceServer.send(400, "text/plain", "Missing name or type"); return;
  }
  String name = deviceServer.arg("name");
  name.trim();
  int type = deviceServer.arg("type").toInt();
  if (name.length() == 0 || type < USER_SWITCH || type > USER_TELEVISION) {
    deviceServer.send(400, "text/plain; charset=utf-8", "设备名称或类型无效"); return;
  }
  for (uint8_t id = 0; id < MAX_USER_DEVICES; ++id) {
    UserDevice existing;
    if (loadUserDevice(id, existing)) continue;
    UserDevice device = {};
    device.magic = DEVICE_MAGIC;
    device.schema = DEVICE_SCHEMA_VERSION;
    device.type = type;
    name.toCharArray(device.name, sizeof(device.name));
    if (!saveUserDevice(id, device)) {
      deviceServer.send(500, "text/plain; charset=utf-8", "保存失败"); return;
    }
    deviceServer.sendHeader("Location", "/");
    deviceServer.send(303);
    return;
  }
  deviceServer.send(409, "text/plain; charset=utf-8", "最多保存 8 个自定义设备");
}

void handleLearn() {
  int id = deviceServer.arg("id").toInt();
  int action = deviceServer.arg("action").toInt();
  UserDevice device;
  if (id < 0 || id >= MAX_USER_DEVICES || action < 0 || action >= MAX_DEVICE_ACTIONS ||
      !loadUserDevice(id, device)) {
    deviceServer.send(400, "text/plain; charset=utf-8", "无效设备或按键"); return;
  }
  webLearningDevice = id;
  webLearningAction = action;
  String html = pageHeader();
  html += "<section><h2>正在学习：" + htmlEscape(device.name) + " / " + actionName(action) + "</h2>";
  html += F("<p>请在 20 厘米内将遥控器对准接收头，并按一次对应按键。</p><p id='state' class='muted'>等待红外信号…</p>"
            "<p><a href='/'>返回设备列表</a></p><script>setInterval(async()=>{let r=await fetch('/status');let j=await r.json();"
            "if(!j.learning){document.getElementById('state').textContent='学习完成，正在返回…';setTimeout(()=>location='/',800)}},700)</script></section></body></html>");
  deviceServer.send(200, "text/html; charset=utf-8", html);
}

void handlePublish() {
  int id = deviceServer.arg("id").toInt();
  UserDevice device;
  if (id < 0 || id >= MAX_USER_DEVICES || !loadUserDevice(id, device)) {
    deviceServer.send(400, "text/plain; charset=utf-8", "无效设备"); return;
  }
  device.enabled = 1;
  if (!saveUserDevice(id, device)) {
    deviceServer.send(500, "text/plain; charset=utf-8", "保存失败"); return;
  }
  homeSpan.forceNewConfigNumber();
  restartAt = millis() + 1200;
  deviceServer.send(200, "text/html; charset=utf-8", pageHeader() +
                    "<section><h2>已发布</h2><p>Hub 正在重启并更新 HomeKit 配置。请稍候。</p></section></body></html>");
}

void beginDeviceManager() {
  deviceServer.on("/", HTTP_GET, sendManagerPage);
  deviceServer.on("/create", HTTP_POST, handleCreateDevice);
  deviceServer.on("/learn", HTTP_POST, handleLearn);
  deviceServer.on("/publish", HTTP_POST, handlePublish);
  deviceServer.on("/status", HTTP_GET, []() {
    deviceServer.send(200, "application/json", isWebLearning() ? "{\"learning\":true}" : "{\"learning\":false}");
  });
  deviceServer.begin();
  Serial.println("Device manager: http://<Hub IP>:8080");
}

void pollDeviceManager() {
  deviceServer.handleClient();
  if (restartAt != 0 && (int32_t)(millis() - restartAt) >= 0) ESP.restart();
}

bool isWebLearning() { return webLearningDevice >= 0; }

void saveWebLearnedCode(const IRData &signal) {
  LearnedCode code = {};
  code.magic = CODE_MAGIC;
  code.protocol = signal.protocol;
  code.address = signal.address;
  code.command = signal.command;
  code.extra = signal.extra;
  code.rawCode = signal.decodedRawData;
  code.bits = signal.numberOfBits;
  char key[12];
  deviceCodeKey(webLearningDevice, webLearningAction, key, sizeof(key));
  if (preferences.putBytes(key, &code, sizeof(code)) == sizeof(code))
    Serial.printf("Web learning saved: device=%u action=%u\n", webLearningDevice + 1, webLearningAction);
  else
    Serial.println("Web learning could not save the command.");
  webLearningDevice = -1;
  webLearningAction = -1;
}

struct IRLearnedSwitch : Service::Switch {
  uint8_t id;
  SpanCharacteristic *on;
  IRLearnedSwitch(uint8_t id) : Service::Switch(), id(id) { on = new Characteristic::On(0); }
  boolean update() override { return !on->updated() || sendUserCode(id, ACTION_POWER); }
};

struct IRLearnedFan : Service::Fan {
  uint8_t id;
  SpanCharacteristic *active;
  SpanCharacteristic *speed;
  IRLearnedFan(uint8_t id) : Service::Fan(), id(id) {
    active = new Characteristic::Active(0);
    speed = new Characteristic::RotationSpeed(50);
  }
  boolean update() override {
    if (active->updated()) return sendUserCode(id, ACTION_POWER);
    if (speed->updated()) return sendUserCode(id, speed->getNewVal<float>() > speed->getVal<float>() ? ACTION_SPEED_UP : ACTION_SPEED_DOWN);
    return true;
  }
};

struct IRLearnedTelevision : Service::Television {
  uint8_t id;
  SpanCharacteristic *active;
  SpanCharacteristic *remoteKey;
  IRLearnedTelevision(uint8_t id) : Service::Television(), id(id) {
    active = new Characteristic::Active(0);
    remoteKey = new Characteristic::RemoteKey();
  }
  boolean update() override {
    if (active->updated()) return sendUserCode(id, ACTION_POWER);
    if (!remoteKey->updated()) return true;
    switch (remoteKey->getNewVal()) {
      case 4: return sendUserCode(id, ACTION_UP);
      case 5: return sendUserCode(id, ACTION_DOWN);
      case 6: return sendUserCode(id, ACTION_LEFT);
      case 7: return sendUserCode(id, ACTION_RIGHT);
      case 8: return sendUserCode(id, ACTION_SELECT);
      case 11: return sendUserCode(id, ACTION_CHANNEL_UP);
      case 12: return sendUserCode(id, ACTION_CHANNEL_DOWN);
      default: return true;
    }
  }
};

struct IRLearnedTelevisionSpeaker : Service::TelevisionSpeaker {
  uint8_t id;
  SpanCharacteristic *volumeSelector;
  IRLearnedTelevisionSpeaker(uint8_t id) : Service::TelevisionSpeaker(), id(id) {
    new Characteristic::VolumeControlType(1);
    volumeSelector = new Characteristic::VolumeSelector();
  }
  boolean update() override {
    return !volumeSelector->updated() || sendUserCode(id, volumeSelector->getNewVal() == 0 ? ACTION_VOLUME_UP : ACTION_VOLUME_DOWN);
  }
};

void configureUserDevices() {
  for (uint8_t id = 0; id < MAX_USER_DEVICES; ++id) {
    UserDevice device;
    if (!loadUserDevice(id, device) || !device.enabled) continue;
    new SpanAccessory();
      new Service::AccessoryInformation();
        new Characteristic::Identify();
        new Characteristic::Name(device.name);
        new Characteristic::Manufacturer("ESP32 IR Hub");
        new Characteristic::Model("Learned IR Device");
        new Characteristic::FirmwareRevision(FIRMWARE_VERSION);
      if (device.type == USER_SWITCH) {
        new IRLearnedSwitch(id);
      } else if (device.type == USER_FAN) {
        new IRLearnedFan(id);
      } else {
        SpanService *speaker = new IRLearnedTelevisionSpeaker(id);
        (new IRLearnedTelevision(id))->addLink(speaker);
      }
  }
}
