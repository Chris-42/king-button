#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>
#include "cmd_processor.h"

AsyncMqttClient mqttClient;
CMD_PROCESSOR cmd_processor = CMD_PROCESSOR();

#define BUTTON_PIN D0
#define BUTTON_PIN_BITMASK (1ULL << GPIO_NUM_0) // GPIO 0 bitmask for ext1
#define FactorSeconds 1000000ULL
#define WIFI_WAIT_TIME 10000

struct systemconfig_t {
  bool valid;
  char hostname[32];
  char ssid[32];
  char wifi_pw[32];
  char mqtt_server[32];
  uint16_t mqtt_server_port;
  char mqtt_topic[40];
  uint16_t retry;
  uint16_t interval;
  float voltage_faktor;
  uint16_t id;
};
RTC_DATA_ATTR struct systemconfig_t sys_config;

bool uart_avail = false;;
float voltage;
JsonDocument ap_list;              // json access point list from scan
std::vector<int> mqtt_pkt_ids;     // list of published packets
volatile bool mqtt_queued = false; //set if mqtt client has published async

RTC_DATA_ATTR int wifi_failed_ct;
RTC_DATA_ATTR bool last_send_failed;
RTC_DATA_ATTR int button_wakeups;

#ifdef DEBUG
#define Debugprint(...) Serial0.print(__VA_ARGS__)
#define Debugprintln(...) Serial0.println(__VA_ARGS__)
#define Debugprintf(...) Serial0.printf(__VA_ARGS__)
#else
#define Debugprint(...)
#define Debugprintln(...)
#define Debugprintf(...)
#endif

bool get_system_config(struct systemconfig_t *sys_cfg) {
  Debugprint("load syscfg ");
  Preferences prefs;
  if(prefs.begin("config")) {
    size_t len = prefs.getBytesLength("system");
    if(len > 0) {
      prefs.getBytes("system", sys_cfg, len);
    }
    prefs.end();
  } else {
    sys_cfg->valid = false;
    Debugprintln("failed");
    return false;
  }
  Debugprintln("ok");
  return true;
}

bool store_system_config(struct systemconfig_t *sys_cfg) {
  Preferences prefs;
  if(!prefs.begin("config")) {
    return false;
  }  
  prefs.clear();
  sys_cfg->valid = true;
  if(prefs.putBytes("system", sys_cfg, sizeof(systemconfig_t)) != sizeof(systemconfig_t)) {
    prefs.clear();
    prefs.end();
    return false;
  }
  prefs.end();
  return true;
}

void goToSleep(int seconds) {
  if(seconds) {
    esp_sleep_enable_timer_wakeup(FactorSeconds * seconds);
  }
  //esp_deep_sleep_enable_gpio_wakeup(BUTTON_PIN_BITMASK, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_sleep_enable_ext1_wakeup(BUTTON_PIN_BITMASK,ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_disable_rom_logging();
  digitalWrite(LED_BUILTIN, HIGH);
  Debugprintf("sleep after %d ms", millis());
  esp_deep_sleep_start();
}

uint16_t mqtt_publish_str(const char* subtopic, const char* data) {
  if(!mqttClient.connected()) {
    return 0;
  }
  char topic[64];
  sprintf(topic, "%s/%d/%s", sys_config.mqtt_topic, sys_config.id, subtopic);
  return mqttClient.publish( topic, 1, false, data);
}

uint16_t mqtt_publish_int(const char* subtopic, int data) {
  char str[32];
  sprintf(str, "%d", data);
  return mqtt_publish_str(subtopic, str);
}

uint16_t mqtt_publish_float(const char* subtopic, float data) {
  char str[32];
  sprintf(str, "%.2f", data);
  return mqtt_publish_str(subtopic, str);
}

void onMqttPublish(int packet_id) {
  std::erase_if(mqtt_pkt_ids, [packet_id] (const int& id) { return id == packet_id; });
}

void onMqttConnect(bool sessionPresent) {
  char uptime[16];
  char str[16];
  
  Debugprintln("Connected to MQTT.");
  Debugprintf("Session present: %d\r\n", sessionPresent);
  Debugprintf("Button pressed %d\r\n", button_wakeups);
  Debugprintf("voltage: %.2f\r\n", voltage);

  mqtt_pkt_ids.clear();
  mqtt_pkt_ids.push_back(mqtt_publish_float("voltage", voltage));
  mqtt_pkt_ids.push_back(mqtt_publish_int("button_ct", button_wakeups));
  String output;
  serializeJson(ap_list, output);
  mqtt_pkt_ids.push_back(mqtt_publish_str("aps", output.c_str()));
  ap_list["voltage"] = voltage;
  ap_list["button_ct"] = button_wakeups;
  ap_list["id"] = sys_config.id;
  ap_list["wifi_fail"] = wifi_failed_ct;
  serializeJson(ap_list, output);
  int id = mqttClient.publish(sys_config.mqtt_topic, 1, false, output.c_str());
  mqtt_pkt_ids.push_back(id);
  mqtt_queued = true;
}

void sendMqtt() {
  mqttClient.setServer(sys_config.mqtt_server, sys_config.mqtt_server_port);
  mqttClient.onConnect(onMqttConnect);
  mqttClient.onPublish(onMqttPublish);
  mqttClient.connect();
}

void WiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch(event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Debugprintln("WiFi connected");
      Debugprint("IP address: ");
      Debugprintln(IPAddress(info.got_ip.ip_info.ip.addr));
      Debugprint("Hostname: ");
      Debugprintln(WiFi.getHostname());
      sendMqtt();
      break;
    case ARDUINO_EVENT_WIFI_SCAN_DONE:
      if(info.wifi_scan_done.status == 0) {
        Debugprintf("scan ok: %d APs\r\n", info.wifi_scan_done.number);
        ap_list.clear();
        for(int i = 0; i < info.wifi_scan_done.number; ++i) {
          ap_list["ap"][i]["ssid"] = WiFi.SSID(i);
          ap_list["ap"][i]["bssid"] = WiFi.BSSIDstr(i);
          ap_list["ap"][i]["rssi"] = WiFi.RSSI(i);
          ap_list["ap"][i]["channel"] = WiFi.channel(i);
        }
      } else {
        Debugprintln("scan failed");
      }
      #ifdef DEBUG
      {
        String output;
        serializeJson(ap_list, output);
        Debugprintf("%s\r\n", output.c_str());
      }
      #endif
      WiFi.begin(sys_config.ssid, sys_config.wifi_pw);
      break;
    default:
      Debugprintln("unhandled Wifi event");
  }
}

void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(sys_config.hostname);
  WiFi.onEvent(WiFiEvent, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(WiFiEvent, WiFiEvent_t::ARDUINO_EVENT_WIFI_SCAN_DONE);
  //WiFi.begin(sys_config.ssid, sys_config.wifi_pw);
  WiFi.scanNetworks(true);
}

void handleCmd(String &cmd) {
  Debugprintf("cmd:'%s'\r\n", cmd.c_str());
  if(cmd == "reset") {
    Serial.println();
    esp_restart();
  } else if(cmd.startsWith("ssid ")) {
    char str[128];
    if(sscanf(cmd.c_str(), "ssid %127s", str) != 1) {
      Serial.println("ssid <ssid>");
      return;
    }
    Serial.println();
    strncpy(sys_config.ssid, str, sizeof(sys_config.ssid));
  } else if(cmd.startsWith("pw ")) {
    char str[128];
    if(sscanf(cmd.c_str(), "pw %127s", str) != 1) {
      Serial.println("pw <password>");
      return;
    }
    Serial.println();
    strncpy(sys_config.wifi_pw, str, sizeof(sys_config.wifi_pw));
  } else if(cmd.startsWith("name ")) {
    char str[128];
    if(sscanf(cmd.c_str(), "name %127s", str) != 1) {
      Serial.println("name <hostname>");
      return;
    }
    Serial.println();
    strncpy(sys_config.hostname, str, sizeof(sys_config.hostname));
  } else if(cmd.startsWith("server ")) {
    char str[128];
    if(sscanf(cmd.c_str(), "server %127s", str) != 1) {
      Serial.println("server <mqtt server>");
      return;
    }
    Serial.println();
    strncpy(sys_config.mqtt_server, str, sizeof(sys_config.mqtt_server));
  } else if(cmd.startsWith("topic ")) {
    char str[128];
    if(sscanf(cmd.c_str(), "topic %127s", str) != 1) {
      Serial.println("topic <base topic>");
      return;
    }
    Serial.println();
    strncpy(sys_config.mqtt_topic, str, sizeof(sys_config.mqtt_topic));
  } else if(cmd.startsWith("port ")) {
    int val;
    if(sscanf(cmd.c_str(), "port %d", &val) != 1) {
      Serial.println("port <mqtt port>");
      return;
    }
    Serial.println();
    sys_config.mqtt_server_port = val;
  } else if(cmd.startsWith("retry ")) {
    int val;
    if(sscanf(cmd.c_str(), "retry %d", &val) != 1) {
      Serial.println("retry <time>");
      return;
    }
    Serial.println();
    sys_config.retry = val;
  } else if(cmd.startsWith("interval ")) {
    int val;
    if(sscanf(cmd.c_str(), "interval %d", &val) != 1) {
      Serial.println("interval <time>");
      return;
    }
    Serial.println();
    sys_config.interval = val;
  } else if(cmd.startsWith("id ")) {
    int val;
    if(sscanf(cmd.c_str(), "id %d", &val) != 1) {
      Serial.println("id <number>");
      return;
    }
    Serial.println();
    sys_config.id = val;
  } else if(cmd.startsWith("volt ")) {
    float v;
    if(sscanf(cmd.c_str(), "volt %f", &v) != 1) {
      Serial.println("volt <measured voltage>");
      return;
    }
    Serial.println();
    sys_config.voltage_faktor = v / analogReadMilliVolts(6);
  } else if(cmd.startsWith("sleep ")) {
    int val;
    if(sscanf(cmd.c_str(), "sleep %d", &val) != 1) {
      Serial.println("sleep <time>");
      return;
    }
    Serial.println();
    goToSleep(val);
  } else if(cmd == "show") {
    Serial.println();
    Serial.printf("ssid %s\r\n", sys_config.ssid);
    Serial.printf("pw %s\r\n", sys_config.wifi_pw);
    Serial.printf("name %s\r\n", sys_config.hostname);
    Serial.printf("id %d\r\n", sys_config.id);
    Serial.printf("server %s\r\n", sys_config.mqtt_server);
    Serial.printf("port %d\r\n", sys_config.mqtt_server_port);
    Serial.printf("topic %s\r\n", sys_config.mqtt_topic);
    Serial.printf("retry %d\r\n", sys_config.retry);
    Serial.printf("interval %d\r\n", sys_config.interval);
    Serial.printf("volt f %.3f\r\n", sys_config.voltage_faktor * 1000.0);
  } else if(cmd == "save") {
    if(store_system_config(&sys_config)) {
      Serial.println(" ok");
    } else {
      Serial.println(" failed");
    }
  } else if(cmd == "send") {
    Serial.println();
    sendMqtt();
  } else if(cmd == "v") {
    Serial.printf(" %.2f\r\n", voltage);
  } else if(cmd == "") {
    Serial.println();
  } else {
    Serial.println(" ?" + cmd + "?");
  }
}

void setup() {
  setCpuFrequencyMhz(80);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  digitalWrite(LED_BUILTIN, LOW);
  Serial0.begin(115200);
  Debugprintln("Init");
  esp_sleep_wakeup_cause_t wakeup_reason;
  wakeup_reason = esp_sleep_get_wakeup_cause();
  //on reset
  if(wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
    Debugprintln("reset");
  } else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    Debugprintln("TIMER wakeup");
  } else if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
    Debugprintln("EXT1 wakeup");
    button_wakeups++;
  } else {
    Debugprintf("wakeup %d\n", wakeup_reason);
  }
  if((wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) ||
     ((wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) && !digitalRead(BUTTON_PIN))) {
    Serial.begin(115200);
    int ct = 20;
    while(!Serial && ct) {
      ct--;
      delay(10);
    }
    if(ct) {
      uart_avail = true;
      Debugprintln("usb uart ok");
    } else {
      Debugprintln("no usb uart");
    }
  }
  if(!sys_config.valid) {
    if(!get_system_config(&sys_config)) {
      sys_config = {.mqtt_server_port = 1883, .retry = 30, .interval = 7200, .voltage_faktor = 0.002};
    }
  }
  if(sys_config.valid) {
    startWifi();
  }
  if(uart_avail) {
    cmd_processor.registerCmd("", handleCmd);
  }
  voltage = sys_config.voltage_faktor * analogReadMilliVolts(6);
  Debugprintln("Init end");
}

uint32_t last_blink = 0;
void loop() {
  uint32_t ti = millis();
  if((ti - last_blink) > 100) {
    last_blink = ti;
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    if(!uart_avail && Serial) {
      Debugprintln("detected serial");
      uart_avail = true;
    }
    if(uart_avail && !Serial) {
      Debugprintln("serial disconnect");
      uart_avail = false;
      goToSleep(sys_config.retry);
    }
  }

  if(mqtt_queued && !mqtt_pkt_ids.size()) {
    Debugprintln("mqtt fin");
    mqtt_queued = false;
    button_wakeups = 0;
    last_send_failed = false;
    if(!uart_avail) {
      goToSleep(sys_config.interval);
    }
  }

  // wait max 10 sec until sleep and retry
  if(!uart_avail && (ti > WIFI_WAIT_TIME)) {
    wifi_failed_ct++;
    last_send_failed = true;
  }

  if(uart_avail) {
    cmd_processor.process();
  }
}

