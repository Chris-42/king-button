#include <Arduino.h>
#include <WiFi.h>
#include "my_Network.h"

#define BUTTON_PIN_BITMASK (1ULL << GPIO_NUM_0) // GPIO 0 bitmask for ext1
#define LED_PIN 15
#define FactorSeconds 1000000ULL

const char* hostname = HOSTNAME;
const char* ssid = MY_SSID;
const char* password = MY_SSID_PW;
const char* mqtt_server = MQTT_SERVER; 
bool uart_avail;
uint16_t voltage;
bool sleep_delayed = false;

RTC_DATA_ATTR bool last_wifi_failed;
RTC_DATA_ATTR bool last_send_failed;
RTC_DATA_ATTR int button_wakeups;
uint32_t wait_time_us = 10000;

#define Serialprint(...) if(uart_avail) Serial.print(__VA_ARGS__)
#define Serialprintln(...) if(uart_avail) Serial.println(__VA_ARGS__)
#define Serialprintf(...) if(uart_avail) Serial.printf(__VA_ARGS__)

void goToSleep(int seconds) {
  if(seconds) {
    esp_sleep_enable_timer_wakeup(FactorSeconds * seconds);
  }
  //esp_deep_sleep_enable_gpio_wakeup(BUTTON_PIN_BITMASK, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_sleep_enable_ext1_wakeup(BUTTON_PIN_BITMASK,ESP_EXT1_WAKEUP_ANY_LOW);
  esp_deep_sleep_disable_rom_logging();
  digitalWrite(LED_PIN, HIGH);
  Serialprintln("sleep");
  esp_deep_sleep_start();
}

void sendFunction() {
  // send code
  Serialprintf("Button pressed %d\r\n", button_wakeups);
  Serialprintf("voltage: %d\r\n", voltage);
  bool success = false;
  if(!sleep_delayed) {
    if(success) {
      goToSleep(86400);
      last_send_failed = false;
    } else {
      last_send_failed = true;
      goToSleep(60);
    }
  }
}

void WiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch(event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serialprintln("WiFi connected");
      Serialprint("IP address: ");
      Serialprintln(IPAddress(info.got_ip.ip_info.ip.addr));
      Serialprint("Hostname: ");
      Serialprintln(WiFi.getHostname());
      last_wifi_failed = false;
      sendFunction();
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serialprintln("Wifi IP lost");
      break;
    default:
      Serialprintln("unhandled Wifi event");
  }
}

void startWifi() {
  WiFi.mode(WIFI_STA);
  if(last_wifi_failed) {
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  }
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(hostname);
  WiFi.begin(ssid, password);
  delay(1);
  WiFi.setHostname(hostname);
  WiFi.onEvent(WiFiEvent, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  pinMode(9, INPUT_PULLUP);
  digitalWrite(LED_PIN, LOW);
  int ct = 100;
  Serial.begin(115200);
  if(!digitalRead(9)) {
    while(!Serial) {
    };
  }
  // check if a serial is connected to avoid printing if not
  int serial_queue_length = Serial.availableForWrite(); // get serial queue length
  Serial.println("Initialization start serial test"); //keep long to allow writability detection
  delay(10); //allow serial to write
  Serial.println((serial_queue_length - Serial.availableForWrite()));
  if((serial_queue_length - Serial.availableForWrite()) < 20) { //at least some chars written out from queue
    Serial.printf("Ser %d %d\r\n", serial_queue_length, Serial.availableForWrite());
    Serial.println("uart ok");
    uart_avail = true;
  }
  Serialprintln("Init");
  esp_sleep_wakeup_cause_t wakeup_reason;
  wakeup_reason = esp_sleep_get_wakeup_cause();
  //on reset
  if(wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
    Serialprintln("reset, waiting 60 sec");
    wait_time_us = 60000;
    sleep_delayed = true;
  } else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    Serialprintf("TIMER wakeup\n");
  } else if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
    Serialprintf("EXT1 wakeup\n");
    button_wakeups++;
  } else {
    Serialprintf("wakeup %d\n", wakeup_reason);
  }
  startWifi();
  voltage = analogRead(1);
  Serialprintln("Init end");
}

uint32_t last_blink = 0;
void loop() {
  uint32_t ti = millis();
  if((ti - last_blink) > 100) {
    last_blink = ti;
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
  // wait max 10 sec until sleep and retry
  if(ti > wait_time_us) {
    last_wifi_failed = true;
    goToSleep(60);
  }
}

