// Dynamic learned devices, their HomeKit services, and the local setup page.

WebServer deviceServer(8080);
int8_t webLearningDevice = -1;
int8_t webLearningAction = -1;
uint32_t restartAt = 0;

constexpr uint32_t BUILTIN_CONFIG_MAGIC = 0x49424332;  // "IBC2"
enum BuiltInDevice : uint8_t { BUILTIN_AIR_CONDITIONER, BUILTIN_TELEVISION };
struct __attribute__((packed)) BuiltInConfig {
  uint32_t magic;
  uint8_t airConditionerEnabled;
  uint8_t televisionEnabled;
  uint8_t setTopBoxCarrier;
  char airConditionerName[32];
  char televisionName[32];
};
BuiltInConfig builtInConfig = {};

void setSetTopBoxCarrier(uint8_t carrier);
void sendWebAirConditionerTest(uint8_t action);
void sendWebTelevisionTest(uint8_t action);

bool saveBuiltInDeviceConfig() {
  return preferences.putBytes("builtins", &builtInConfig, sizeof(builtInConfig)) ==
         sizeof(builtInConfig);
}

void loadBuiltInDeviceConfig() {
  if (preferences.getBytesLength("builtins") == sizeof(builtInConfig))
    preferences.getBytes("builtins", &builtInConfig, sizeof(builtInConfig));
  if (builtInConfig.magic != BUILTIN_CONFIG_MAGIC) {
    builtInConfig = {};
    builtInConfig.magic = BUILTIN_CONFIG_MAGIC;
    builtInConfig.airConditionerEnabled = 1;
    builtInConfig.televisionEnabled = 1;
    builtInConfig.setTopBoxCarrier = 38;
    strncpy(builtInConfig.airConditionerName, "美菱空调", sizeof(builtInConfig.airConditionerName) - 1);
    strncpy(builtInConfig.televisionName, "Pioneer机顶盒", sizeof(builtInConfig.televisionName) - 1);
    saveBuiltInDeviceConfig();
  }
  setSetTopBoxCarrier(builtInConfig.setTopBoxCarrier);
}

bool isBuiltInDeviceEnabled(uint8_t device) {
  return device == BUILTIN_AIR_CONDITIONER ? builtInConfig.airConditionerEnabled
                                            : builtInConfig.televisionEnabled;
}

const char *builtInDeviceName(uint8_t device) {
  return device == BUILTIN_AIR_CONDITIONER ? builtInConfig.airConditionerName
                                            : builtInConfig.televisionName;
}

uint8_t builtInSetTopBoxCarrier() { return builtInConfig.setTopBoxCarrier; }

void setBuiltInSetTopBoxCarrier(uint8_t carrier) {
  builtInConfig.setTopBoxCarrier = carrier;
  saveBuiltInDeviceConfig();
}

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
    case USER_AIR_CONDITIONER: return "空调";
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
    case ACTION_TEMPERATURE_UP: return "温度增加";
    case ACTION_TEMPERATURE_DOWN: return "温度降低";
    case ACTION_MODE: return "模式切换";
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
  static const uint8_t airConditionerActions[] = {
      ACTION_POWER, ACTION_TEMPERATURE_UP, ACTION_TEMPERATURE_DOWN,
      ACTION_MODE, ACTION_SPEED_UP, ACTION_SPEED_DOWN};
  if (type == USER_SWITCH) { actions = switchActions; return 1; }
  if (type == USER_FAN) { actions = fanActions; return 3; }
  if (type == USER_AIR_CONDITIONER) { actions = airConditionerActions; return 6; }
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
  return F("<!doctype html><html lang='zh-CN'><meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>"
           "<title>红外 Hub · 家居控制</title><style>"
           ":root{--ink:#26342e;--muted:#728077;--sage:#718c7b;--sage-dark:#557062;--sage-pale:#dfe8e0;--canvas:#eef2ed;--card:rgba(255,255,252,.84);--line:#d5dfd6;--warm:#fafaf5}"
           "*{box-sizing:border-box}body{font-family:-apple-system,BlinkMacSystemFont,'PingFang SC','Noto Sans SC',sans-serif;max-width:860px;margin:0 auto;padding:calc(28px + env(safe-area-inset-top)) 20px calc(68px + env(safe-area-inset-bottom));color:var(--ink);background:linear-gradient(rgba(238,242,237,.82),rgba(238,242,237,.9)),url('/home-bg.jpg') center/cover fixed,var(--canvas);letter-spacing:.01em}"
           ".hero{padding:34px 30px 31px;border-radius:24px;background:linear-gradient(132deg,rgba(255,255,251,.94),rgba(224,235,225,.9));border:1px solid rgba(255,255,255,.8);box-shadow:0 14px 34px rgba(55,76,62,.1);position:relative;overflow:hidden}.hero:after{content:'';position:absolute;width:190px;height:190px;border:1px solid rgba(91,125,102,.2);border-radius:50%;right:-80px;top:-95px;box-shadow:-25px 27px 0 -1px rgba(91,125,102,.09)}"
           ".eyebrow{margin:0 0 9px;color:var(--sage-dark);font-size:12px;font-weight:700;letter-spacing:.14em}.hero h1{margin:0;font-family:ui-serif,Georgia,'Songti SC',serif;font-size:30px;letter-spacing:.04em;font-weight:600}.hero p{max-width:530px;margin:10px 0 0;color:var(--muted);font-size:14px;line-height:1.65;position:relative;z-index:1}"
           "section{border:1px solid var(--line);border-radius:20px;padding:27px 25px;margin:22px 0;background:var(--card);backdrop-filter:blur(7px);box-shadow:0 8px 24px rgba(45,65,52,.07)}section h2{font-family:ui-serif,Georgia,'Songti SC',serif;font-size:21px;font-weight:600;letter-spacing:.02em;margin:0 0 14px}section h2 small{font-family:-apple-system,BlinkMacSystemFont,'PingFang SC',sans-serif;color:var(--muted);font-size:12px;font-weight:500;letter-spacing:0}.muted{color:var(--muted);font-size:14px;line-height:1.8}.ok{color:var(--sage-dark);font-size:14px;font-weight:600}"
           "form{display:flex;flex-wrap:wrap;align-items:center;gap:10px;margin:16px 0}.config-form>input[name=name]{flex:1 1 220px}.action-form{display:inline-flex;margin:8px 8px 0 0}.publish-form{margin-top:14px}.danger{background:#8d5b58}button{appearance:none;background:var(--sage-dark);color:#fff;border:0;border-radius:10px;padding:11px 16px;min-height:42px;margin:0;font:600 14px -apple-system,BlinkMacSystemFont,'PingFang SC',sans-serif;letter-spacing:.01em;box-shadow:0 3px 7px rgba(51,76,61,.14)}button:active{transform:translateY(1px);background:#415b4d}input,select{font:15px -apple-system,BlinkMacSystemFont,'PingFang SC',sans-serif;color:var(--ink);background:#fbfcf9;border:1px solid #cbd8cc;border-radius:9px;padding:11px 12px;min-height:42px;margin:0;max-width:100%}input[type=checkbox]{appearance:auto;accent-color:var(--sage-dark);width:22px!important;height:22px;min-width:22px!important;min-height:22px;flex:0 0 22px!important;padding:0;vertical-align:middle}label{display:inline-flex;align-items:center;gap:8px;font-size:14px;color:var(--muted);white-space:nowrap}a{color:var(--sage-dark);font-weight:600;text-decoration:none}@media(max-width:520px){body{padding:calc(22px + env(safe-area-inset-top)) 13px calc(45px + env(safe-area-inset-bottom))}.hero{padding:27px 22px}.hero h1{font-size:27px}section{padding:22px 18px;margin:17px 0}.config-form,.create-form{flex-direction:column;align-items:stretch;gap:12px}.config-form>input[name=name],.config-form select,.config-form button,.create-form input[name=name],.create-form select,.create-form button{width:100%;flex:0 0 auto}.config-form label{width:100%;min-height:42px}.action-form{display:flex;width:100%;margin:8px 0 0}.action-form button,.publish-form button{width:100%}button{padding:11px 13px;font-size:14px}}"
           "</style><body><header class='hero'><p class='eyebrow'>IR HUB · HOME EDITION</p><h1>家居控制中心</h1><p>把熟悉的遥控器，安静地收进你的日常生活。</p></header>");
}

void sendManagerPage() {
  String html = pageHeader();
  html += F("<p class='muted'>在此管理内置与学习型遥控器。修改 HomeKit 配件名称或启用状态后，Hub 将自动重启并刷新家庭 App。</p>");
  html += "<section><h2>" + htmlEscape(builtInDeviceName(BUILTIN_AIR_CONDITIONER)) + " <small>（内置：美菱 KKZM-4WB 空调）</small></h2>";
  html += F("<p class='muted'>协议：38 kHz、120 bit；网页可测试开机、关机、显示屏切换。温度、模式及风速仍通过 Home App 控制。</p>");
  html += "<form class='config-form' method='post' action='/builtin'><input type='hidden' name='id' value='0'><input name='name' maxlength='31' value='" + htmlEscape(builtInDeviceName(BUILTIN_AIR_CONDITIONER)) + "'>";
  html += "<label><input type='checkbox' name='enabled'" + String(builtInConfig.airConditionerEnabled ? " checked" : "") + ">显示在 HomeKit</label><button>保存空调配置</button></form>";
  html += F("<form class='action-form' method='post' action='/builtintest'><input type='hidden' name='id' value='0'><input type='hidden' name='action' value='0'><button>测试开机</button></form>"
            "<form class='action-form' method='post' action='/builtintest'><input type='hidden' name='id' value='0'><input type='hidden' name='action' value='1'><button>测试关机</button></form>"
            "<form class='action-form' method='post' action='/builtintest'><input type='hidden' name='id' value='0'><input type='hidden' name='action' value='2'><button>测试显示屏</button></form></section>");
  html += "<section><h2>" + htmlEscape(builtInDeviceName(BUILTIN_TELEVISION)) + " <small>（内置：Pioneer 机顶盒）</small></h2>";
  html += F("<p class='muted'>协议：Pioneer NEC 与原始机顶盒按键时序；可选择原始时序载波频率。</p>");
  html += "<form class='config-form' method='post' action='/builtin'><input type='hidden' name='id' value='1'><input name='name' maxlength='31' value='" + htmlEscape(builtInDeviceName(BUILTIN_TELEVISION)) + "'>";
  html += "<label><input type='checkbox' name='enabled'" + String(builtInConfig.televisionEnabled ? " checked" : "") + ">显示在 HomeKit</label> <select name='carrier'>";
  const uint8_t carriers[] = {38, 40, 36, 56};
  for (uint8_t carrier : carriers) {
    html += "<option value='" + String(carrier) + "'" + String(builtInSetTopBoxCarrier() == carrier ? " selected" : "") + ">" + String(carrier) + " kHz</option>";
  }
  html += F("</select><button>保存机顶盒配置</button></form>"
            "<form class='action-form' method='post' action='/builtintest'><input type='hidden' name='id' value='1'><input type='hidden' name='action' value='0'><button>测试电源</button></form>"
            "<form class='action-form' method='post' action='/builtintest'><input type='hidden' name='id' value='1'><input type='hidden' name='action' value='1'><button>测试音量+</button></form>"
            "<form class='action-form' method='post' action='/builtintest'><input type='hidden' name='id' value='1'><input type='hidden' name='action' value='2'><button>测试音量-</button></form></section>");
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
      html += "<form class='action-form' method='post' action='/learn'><input type='hidden' name='id' value='" + String(id) + "'><input type='hidden' name='action' value='" + String(actions[i]) + "'><button>";
      html += learned ? "重新学习：" : "学习：";
      html += actionName(actions[i]);
      html += "</button></form>";
    }
    if (!device.enabled) {
      html += "<form class='publish-form' method='post' action='/publish'><input type='hidden' name='id' value='" + String(id) + "'><button>完成并发布此设备</button></form>";
    }
    html += "<form class='publish-form' method='post' action='/delete' onsubmit=\"return confirm('确定删除此设备及全部学习码？')\"><input type='hidden' name='id' value='" + String(id) + "'><button class='danger'>删除此设备</button></form>";
    html += "</section>";
  }
  html += F("<section><h2>新建设备</h2><form class='create-form' method='post' action='/create'><input name='name' maxlength='31' required placeholder='例如：客厅电视'>"
            "<select name='type'><option value='0'>普通开关</option><option value='1'>风扇</option><option value='2'>电视</option><option value='3'>空调</option></select><button>创建并开始学习</button></form>"
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

void handleDeleteDevice() {
  int id = deviceServer.arg("id").toInt();
  UserDevice device;
  if (id < 0 || id >= MAX_USER_DEVICES || !loadUserDevice(id, device)) {
    deviceServer.send(400, "text/plain; charset=utf-8", "无效设备"); return;
  }
  char key[12];
  deviceKey(id, key, sizeof(key));
  preferences.remove(key);
  for (uint8_t action = 0; action < MAX_DEVICE_ACTIONS; ++action) {
    deviceCodeKey(id, action, key, sizeof(key));
    preferences.remove(key);
  }
  homeSpan.forceNewConfigNumber();
  restartAt = millis() + 1200;
  deviceServer.send(200, "text/html; charset=utf-8", pageHeader() +
                    "<section><h2>设备已删除</h2><p>红外学习码已清除，Hub 正在重启并更新 HomeKit 配置。</p></section></body></html>");
}

void handleBuiltInDevice() {
  int id = deviceServer.arg("id").toInt();
  String name = deviceServer.arg("name");
  name.trim();
  if ((id != BUILTIN_AIR_CONDITIONER && id != BUILTIN_TELEVISION) || name.length() == 0) {
    deviceServer.send(400, "text/plain; charset=utf-8", "内置设备配置无效"); return;
  }
  char *targetName = id == BUILTIN_AIR_CONDITIONER ? builtInConfig.airConditionerName : builtInConfig.televisionName;
  name.toCharArray(targetName, 32);
  if (id == BUILTIN_AIR_CONDITIONER) {
    builtInConfig.airConditionerEnabled = deviceServer.hasArg("enabled");
  } else {
    int carrier = deviceServer.arg("carrier").toInt();
    if (carrier != 36 && carrier != 38 && carrier != 40 && carrier != 56) {
      deviceServer.send(400, "text/plain; charset=utf-8", "载波频率无效"); return;
    }
    builtInConfig.televisionEnabled = deviceServer.hasArg("enabled");
    builtInConfig.setTopBoxCarrier = carrier;
    setSetTopBoxCarrier(carrier);
  }
  if (!saveBuiltInDeviceConfig()) {
    deviceServer.send(500, "text/plain; charset=utf-8", "保存失败"); return;
  }
  homeSpan.forceNewConfigNumber();
  restartAt = millis() + 1200;
  deviceServer.send(200, "text/html; charset=utf-8", pageHeader() +
                    "<section><h2>内置设备已保存</h2><p>Hub 正在重启并更新 HomeKit 配置。</p></section></body></html>");
}

void handleBuiltInTest() {
  int id = deviceServer.arg("id").toInt();
  int action = deviceServer.arg("action").toInt();
  if (id == BUILTIN_AIR_CONDITIONER && action >= 0 && action <= 2) {
    sendWebAirConditionerTest(action);
  } else if (id == BUILTIN_TELEVISION && action >= 0 && action <= 2) {
    sendWebTelevisionTest(action);
  } else {
    deviceServer.send(400, "text/plain; charset=utf-8", "测试动作无效"); return;
  }
  deviceServer.sendHeader("Location", "/");
  deviceServer.send(303);
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
  deviceServer.on("/home-bg.jpg", HTTP_GET, []() {
    deviceServer.send_P(200, "image/jpeg", (const char *)home_background_jpeg,
                        home_background_jpeg_len);
  });
  deviceServer.on("/", HTTP_GET, sendManagerPage);
  deviceServer.on("/create", HTTP_POST, handleCreateDevice);
  deviceServer.on("/delete", HTTP_POST, handleDeleteDevice);
  deviceServer.on("/builtin", HTTP_POST, handleBuiltInDevice);
  deviceServer.on("/builtintest", HTTP_POST, handleBuiltInTest);
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

struct IRLearnedAirConditioner : Service::HeaterCooler {
  uint8_t id;
  SpanCharacteristic *active;
  SpanCharacteristic *currentTemperature;
  SpanCharacteristic *currentState;
  SpanCharacteristic *targetState;
  SpanCharacteristic *coolingThreshold;
  SpanCharacteristic *heatingThreshold;
  SpanCharacteristic *speed;

  IRLearnedAirConditioner(uint8_t id) : Service::HeaterCooler(), id(id) {
    active = new Characteristic::Active(0);
    currentTemperature = new Characteristic::CurrentTemperature(24);
    currentState = new Characteristic::CurrentHeaterCoolerState(0);
    targetState = new Characteristic::TargetHeaterCoolerState(2);
    targetState->setValidValues(2, 1, 2);
    coolingThreshold = new Characteristic::CoolingThresholdTemperature(24);
    heatingThreshold = new Characteristic::HeatingThresholdTemperature(24);
    coolingThreshold->setRange(16, 32, 1);
    heatingThreshold->setRange(16, 32, 1);
    new Characteristic::TemperatureDisplayUnits(0);
    speed = new Characteristic::RotationSpeed(50);
  }

  boolean update() override {
    if (active->updated()) {
      if (!sendUserCode(id, ACTION_POWER)) return false;
      currentState->setVal(active->getNewVal() ? 2 : 0);
      return true;
    }
    if (targetState->updated()) return sendUserCode(id, ACTION_MODE);
    if (coolingThreshold->updated() || heatingThreshold->updated()) {
      SpanCharacteristic *changed = coolingThreshold->updated()
          ? coolingThreshold : heatingThreshold;
      float nextTemperature = changed->getNewVal<float>();
      bool up = nextTemperature > changed->getVal<float>();
      if (!sendUserCode(id, up ? ACTION_TEMPERATURE_UP : ACTION_TEMPERATURE_DOWN))
        return false;
      currentTemperature->setVal(nextTemperature);
      if (changed == coolingThreshold) heatingThreshold->setVal(nextTemperature);
      else coolingThreshold->setVal(nextTemperature);
      return true;
    }
    if (speed->updated()) {
      return sendUserCode(id, speed->getNewVal<float>() > speed->getVal<float>()
                                  ? ACTION_SPEED_UP : ACTION_SPEED_DOWN);
    }
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
      } else if (device.type == USER_AIR_CONDITIONER) {
        new IRLearnedAirConditioner(id);
      } else {
        SpanService *speaker = new IRLearnedTelevisionSpeaker(id);
        (new IRLearnedTelevision(id))->addLink(speaker);
      }
  }
}
