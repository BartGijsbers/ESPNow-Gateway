/*
   15-02-2020 Major upgrade on Gateway and sensors
   Implementing: automatically find a gateway and channel independant
   The gateway will broadcast a WIFI SSID. The sensor will search for this and remember macaddr and channel in RTC memory
   Remove the ":" in the MQTT topic name of a sensor. This will make it better to read in the mcsMQTT plugin
   The broadcast SSID will be ESPNOW+macaddres of the gateways
   The sensor will check if bootCount = 0. If that is the case a search for the best gateway will be started
   The sending result of a packet will be recorded: succesCount = 0 and incremented on a miss
   If the succesCount > 10 then a search for the gateway will be started.
   This will increace the battery drainage so you have to monitor this
   Unfortunatelly esp8266 still does not work

   13-10-2018 Using the new versions of the expressif code breaks the receipient of esp8266
   
   14-6-2018 Changed REST api to JSON api
   
   20-12-2017 ESPNow_mqtt_json_gateway_esp32

   Code for a Gateway node between MQTT and ESPNow sensors
   The gateway subscribes to: gatewayName/#
   you must Publish a sensor to the gateway in the format: sensor_mac_addr/updatefreq (data is the amount of seconds a sensor sleeps)
   example: ESPNOWGW/30AEA4044BA0/updatefreq (mac_addr in capitals)
   The gateway will publish based on the sensor mac_addr
   Gateway publishes all json objects send by a sensor
   example: a sensor with mac_addr A0:20:A6:26:74:DD who sends {"temperatuur":21.2,"humidity":56.5}
   will result in the following MQTT publishing:
   A020A62674DD/temperatuur 21.2
   A020A62674DD/humidity 56.5
   A020A62674DD/status 08:10:23   <--- time. This is always added to keep track of the sensors
   If a sensor is not seen for 5 times his update frequency, then the following messages is published
   mac_addr/Status "connection lost"

   Gateway will publish his own status: gatewayName/status

    By publishing an MQTT message to gatewayName/sensor_mac_addr/updatefreq you can change the update time of a sensor

    This update command is placed in a queue and send to the sensor at the moment a message is received
    The updatefreq must always be supply-ed to the gateway.
    This is how the sensortable is build.

    Publishing to gatewayName/sensor_mac_addr/mypublishing will result sending this to the sensor
    gatewayName/A020A62674DD/displaytext "Hello world" will send a JSON string to the sensor
    {"displaytext":"Hello world"}
    Keep in mind that the esp-now packet has a maximum sie of 250.
    So the JSON string cannot be bigger! (and no size checking is done).
    Any additional commands are added to the JSON string
    JSON string is cleared when the sensor has received the JSON string

    Command can be send to control the sensor like:
    gatewayName/sensor_mac_addr/cmd
    101 = sleep (switch wifi en OTA off)
    102 = switch wifi on
    103 = switch wifi and OTA on

    Remarks:
    It only seems to work good if the WiFi channel of the gateway is 1 (don't ask me why)
    If WiFi errors/disconnects are encounterd then the gateway will restart
    Don't add more then 20 sensors to one gateway (gateway will crash)

    The gateway by itself is more or less stateless, so rebooting is no problem

*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_now.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include "..\..\passwords.h"

const char *ssid = SSID; // put in your ssid
const char *password = WIFI_PASSWORD; // put in your wifi password
const char *mqtt_server = MQTT_SERVER_IP; // put in your ip-address 10.1.1.1
const char *gatewayName = "espnowgw2";
const char *homeseer = HOMESEER_IP;
char full_esp_now_ssid[30];

WiFiClient espClient;
PubSubClient client(espClient);
WiFiServer TelnetServer(23);
WiFiClient Telnet;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "nl.pool.ntp.org", 3600, 300000);
WebServer server(80);
esp_now_peer_info_t slave;

struct COMMAND_DATA
{
  char mac[20];
  char lastTimeSeen[20]; // "23:12:59" or "Connection Lost"
  int updateFreq;
  unsigned long epochTime;
  char jsonSentData[249];
  boolean sensorEverSeen;
};
COMMAND_DATA commandQueue[20]; // this tabel holds all the sensors of the gateway services

int numberOfSensors = 0;
boolean flagOTA = false; // If OTA active or not
long lastMsg = -600000;
uint8_t peerAddr[6]; // receiving mac_addr of esp data. Filled by ISR: OnDataRecv
char macStr[18];     // receiving mac_addr string of esp data. Filled by ISR: OnDataRecv
char bs[250];        // receiving sensor data. Filled by ISR: OnDataRecv
char md[250];        // receiving mqtt data. Filled by ISR: mqttCallback
char mt[250];        // receiving mqtt topic. Filled by ISR: mqttCallback
long telnetTimer = -1000; // make sure it fires the first time without waiting
int failedMqttConnect = 0;

volatile boolean espCmdReceived = false;
volatile boolean mqttCmdReceived = false;

void setup()
{
  Serial.begin(115200);
  Serial.println();
  initWifi();
  if (esp_now_init() != 0)
  {
    Serial.println("*** ESP_Now init failed");
    ESP.restart();
  }
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

  client.setServer(mqtt_server, 1883);
  client.setCallback(mqttCallback);

  timeClient.begin();
  delay(10);
  initWebserver();
  initOTA();
  delay(100);
  TelnetServer.begin();
  // TelnetServer.setNoDelay(true);
  delay(100);
  hsWriteLog();
} // end setup
void loop()
{
  if (millis() - telnetTimer > 500)
  { // run each 0.5 seconds
    telnetTimer = millis();
    checkWiFiConnection();
    handleTelnet(); //Telnet process
    if (!client.connected())
    {
      reconnect();
    }
  }
  server.handleClient(); // WebServer process
  client.loop();         // MQTT server process
  if (flagOTA)
    ArduinoOTA.handle(); // OTA process
  if (millis() - lastMsg > 60000)
  { // run each xx seconds
    lastMsg = millis();
    publishGatewayStatus();
    timeClient.update();
  }
  if (espCmdReceived)
  {
    processEspData();
    espCmdReceived = false;
  }
  if (mqttCmdReceived)
  {
    processMqttData();
    mqttCmdReceived = false;
  }
  delay(1);
} // end loop
void checkWiFiConnection()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    // wifi down, reconnect here
    WiFi.begin();
    Serial.println("WiFi discounted, will try to reconnect");
    int WLcount = 0;
    int UpCount = 0;
    while (WiFi.status() != WL_CONNECTED && WLcount < 200)
    {
      delay(100);
      Serial.printf(".");
      if (UpCount >= 60) // just keep terminal from scrolling sideways
      {
        UpCount = 0;
        Serial.printf("\n");
      }
      ++UpCount;
      ++WLcount;
    }
    reconnect();
  }
}
void processEspData()
{
  Telnet.print("ESP-Now Packet Recv from: ");
  Telnet.println(macStr);
  Telnet.print("ESP-Now Packet Data: ");
  Telnet.println(bs);
  StaticJsonBuffer<250> jsonBuffer; // for receiving
  JsonObject &root = jsonBuffer.parseObject(bs);
  if (!root.success())
  {
    Telnet.println("parseObject() failed in parseEspData()");
  }
  else
  {
    // send data to sensor if waiting in queue
    boolean sensorFound = false;
    for (int i = 0; i < numberOfSensors; i++)
    {
      if (strcmp(commandQueue[i].mac, macStr) == 0)
      { // we found the sensor
        sensorFound = true;
        timeClient.getFormattedTime().toCharArray(commandQueue[i].lastTimeSeen, 9);
        commandQueue[i].epochTime = timeClient.getEpochTime();
        commandQueue[i].sensorEverSeen = true;
        root["status"] = commandQueue[i].lastTimeSeen; // add status to received data of sensor
        if (commandQueue[i].jsonSentData[0] != NULL)
        { // do we have to send something back?
          Telnet.print("Active data: ");
          Telnet.println(commandQueue[i].jsonSentData);
          //         Telnet.print("Macaddr: ");
          for (int i = 0; i < 6; ++i)
          {
            slave.peer_addr[i] = peerAddr[i];
            //           Telnet.print(slave.peer_addr[i]);
          }
          //         Telnet.println();
          const uint8_t *peer_addr = slave.peer_addr;
          esp_err_t result = esp_now_send(peer_addr, (uint8_t *)&commandQueue[i].jsonSentData, strlen(commandQueue[i].jsonSentData) + 1);
          Telnet.print("ESP-Now Return packet send Status: ");
          if (result == ESP_OK)
            Telnet.println("Success");
          else if (result == ESP_ERR_ESPNOW_NOT_INIT)
            Telnet.println("ESPNOW not Init.");
          else if (result == ESP_ERR_ESPNOW_ARG)
            Telnet.println("Invalid Argument");
          else if (result == ESP_ERR_ESPNOW_INTERNAL)
            Telnet.println("Internal Error");
          else if (result == ESP_ERR_ESPNOW_NO_MEM)
            Telnet.println("ESP_ERR_ESPNOW_NO_MEM");
          else if (result == ESP_ERR_ESPNOW_NOT_FOUND)
            Telnet.println("Peer not found.");
          else
            Telnet.println("Not sure what happened");
        }
        else
        {
          Telnet.println("mac_addr found but no return command active to send");
        }
      }
    }
    if (sensorFound == false)
    { // sensor not found. we will add it.
      Telnet.println("Found a sensor which we don't know. We will add it.");
      Telnet.print("Macaddr: ");
      Telnet.println(macStr);

      strncpy(commandQueue[numberOfSensors].mac, macStr, 20); // set mac adress
      strcpy(commandQueue[numberOfSensors].lastTimeSeen, "Connection Lost");
      timeClient.getFormattedTime().toCharArray(commandQueue[numberOfSensors].lastTimeSeen, 9);
      commandQueue[numberOfSensors].epochTime = timeClient.getEpochTime();
      commandQueue[numberOfSensors].sensorEverSeen = true;
      const char *command = root["updatefreq"];
      if (command)
        commandQueue[numberOfSensors].updateFreq = atoi(root["updatefreq"]);
      else
        commandQueue[numberOfSensors].updateFreq = 0;

      // add it to the sensor peer list
      slave.channel = 0;
      slave.encrypt = 0;
      // convert StringMac[12] into bytemac[6]
      int ii = 0;
      for (int i = 0; i < 12; i += 2)
      {
        slave.peer_addr[ii] = getVal(macStr[i + 1]) + (getVal(macStr[i]) << 4);
        //      Telnet.print(slave.peer_addr[ii]);
        ii += 1;
      }
      const esp_now_peer_info_t *peer = &slave;
      esp_err_t addStatus = esp_now_add_peer(peer);
      if (addStatus == ESP_OK)
      {
        // Pair success
        Telnet.print("Command: ");
        Telnet.println(commandQueue[numberOfSensors].jsonSentData);
      }
      else if (addStatus == ESP_ERR_ESPNOW_NOT_INIT)
        Telnet.println("ESPNOW Not Init");
      else if (addStatus == ESP_ERR_ESPNOW_ARG)
        Telnet.println("Invalid Argument");
      else if (addStatus == ESP_ERR_ESPNOW_FULL)
        Telnet.println("Peer list full");
      else if (addStatus == ESP_ERR_ESPNOW_NO_MEM)
        Telnet.println("Out of memory");
      else if (addStatus == ESP_ERR_ESPNOW_EXIST)
        Telnet.println("Peer Exists");
      else
        Telnet.println("Not sure what happened");
      numberOfSensors++;
      if (numberOfSensors > 20)
      {
        Telnet.println("Error: more then 20 sensors on the gateway!");
        hsWriteLogError();
        numberOfSensors--;
      }

      return;
    }
    // publish all data from sensor to mqtt
    char mqttPublishName[75] = {0};
    for (auto kv : root)
    {
      mqttPublishName[0] = (char)0;
      strcat(mqttPublishName, macStr);
      strcat(mqttPublishName, "/");
      strcat(mqttPublishName, kv.key);
      Telnet.print("MQTT publish: [");
      Telnet.print(mqttPublishName);
      Telnet.print("] ");
      client.publish(mqttPublishName, kv.value.as<char *>());
      Telnet.println(kv.value.as<char *>());
    }
    jsonBuffer.clear();
  }
}
void sendSensorDataInQueue()
{
  // send data to sensor if waiting in queue
  uint8_t tmpPeerAddr[6];
  boolean sensorFound = false;
  for (int i = 0; i < numberOfSensors; i++)
  {
    if (commandQueue[i].jsonSentData[0] != NULL)
    { // do we have to send something back?
      Telnet.print("Active data: ");
      Telnet.println(commandQueue[i].jsonSentData);
      // Telnet.print("Macaddr: ");
      // convert StringMac[12] into bytemac[6]
      int ii = 0;
      for (int x = 0; x < 12; x += 2)
      {
        tmpPeerAddr[ii] = getVal(commandQueue[i].mac[x + 1]) + (getVal(commandQueue[i].mac[x]) << 4);
        //      Telnet.print(tmpPeerAddr[ii]);
        ii += 1;
      }
      //         Telnet.println();
      const uint8_t *peer_addr = tmpPeerAddr;
      esp_err_t result = esp_now_send(peer_addr, (uint8_t *)&commandQueue[i].jsonSentData, strlen(commandQueue[i].jsonSentData) + 1);
      Telnet.print("ESP-Now Return packet send Status: ");
      if (result == ESP_OK)
        Telnet.println("Success");
      else if (result == ESP_ERR_ESPNOW_NOT_INIT)
        Telnet.println("ESPNOW not Init.");
      else if (result == ESP_ERR_ESPNOW_ARG)
        Telnet.println("Invalid Argument");
      else if (result == ESP_ERR_ESPNOW_INTERNAL)
        Telnet.println("Internal Error");
      else if (result == ESP_ERR_ESPNOW_NO_MEM)
        Telnet.println("ESP_ERR_ESPNOW_NO_MEM");
      else if (result == ESP_ERR_ESPNOW_NOT_FOUND)
        Telnet.println("Peer not found.");
      else
        Telnet.println("Not sure what happened");
    }
    else
    {
      Telnet.println("mac_addr found but no return command active to send");
    }
  }
} // sendSensorDataInQueue
void publishGatewayStatus()
{
  // publish gateway status
  Telnet.println("Waiting for ESP-NOW messages or MQTT commands... 600 sec");

  char timeChr[10];
  String timeStr;
  timeStr = timeClient.getFormattedTime();
  Telnet.print("Current time: ");
  Telnet.println(timeStr);
  for (int i = 0; i < 9; i++)
  {
    timeChr[i] = timeStr[i];
    timeChr[i + 1] = char(0);
  }
  // publish online
  char gatewayStatusTopic[100] = {'\0'};
  strcat(gatewayStatusTopic, gatewayName);
  strcat(gatewayStatusTopic, "/status");
  Telnet.print("MQTT publish: [");
  Telnet.print(gatewayStatusTopic);
  Telnet.println("] Online");
  client.publish(gatewayStatusTopic, "Online");
  // public ip address as hyperlink
  char gatewayIPTopic[100] = {'\0'};
  strcat(gatewayIPTopic, gatewayName);
  strcat(gatewayIPTopic, "/ipaddress");
  char msg[100] = {'\0'};
  strcat(msg, "<a href=\"http://");
  char IP[] = "xxx.xxx.xxx.xxx"; // buffer
  IPAddress ip = WiFi.localIP();
  ip.toString().toCharArray(IP, 16);
  strcat(msg, IP);
  strcat(msg, "\"target=\"_blank\">");
  strcat(msg, IP);
  strcat(msg, "</a>");
  // &hs.setdevicestring(1380,"<a href=""http://192.168.5.161""target=""_blank"">Online</a>",true)
  Telnet.print("MQTT publish: [");
  Telnet.print(gatewayIPTopic);
  Telnet.print("] ");
  Telnet.println(msg);
  client.publish(gatewayIPTopic, msg);
  // publish sensor "Connection Lost"
  for (int i = 0; i < numberOfSensors; i++)
  {
    if (commandQueue[i].sensorEverSeen)
    {
      if ((timeClient.getEpochTime() - commandQueue[i].epochTime) > (5 * commandQueue[i].updateFreq))
      {
        //publish Connection Lost
        Telnet.print("Connection Lost to sensor: ");
        Telnet.println(commandQueue[i].mac);
        strncpy(commandQueue[i].lastTimeSeen, "Connection Lost", 20);
        char mqttPublishName[250] = {'\0'};
        strcat(mqttPublishName, commandQueue[i].mac);
        strcat(mqttPublishName, "/status");
        Telnet.print("MQTT publish: ");
        Telnet.print(mqttPublishName);
        Telnet.print(" ");
        client.publish(mqttPublishName, "Connection Lost");
        Telnet.println("Connection Lost");
        commandQueue[i].sensorEverSeen = false;
      }
    }
  }
} //end publishGatewayStatus
void initWifi()
{
  WiFi.setHostname(gatewayName);
  // IPAddress local_IP(192, 168, 5, 10);
  // IPAddress gateway(192, 168, 5, 1);
  // IPAddress subnet(255, 255, 255, 0);
  // IPAddress primaryDNS(208, 67, 222, 222);
  // IPAddress secondaryDNS(208, 67, 220, 220);
  WiFi.mode(WIFI_AP_STA);
  Serial.print("Connecting to ");
  Serial.print(ssid);
  if (strcmp(WiFi.SSID().c_str(), ssid) != 0)
  {
    WiFi.begin(ssid, password);
  }
  int retries = 20; // 10 seconds
  while ((WiFi.status() != WL_CONNECTED) && (retries-- > 0))
  {
    delay(500);
    Telnet.print(".");
  }
  Serial.println("");
  if (retries < 1)
  {
    Serial.print("*** WiFi connection failed");
    ESP.restart();
  }
  Serial.print("WiFi connected, IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("STA mac: ");
  Serial.println(WiFi.macAddress());
  Serial.print("WiFi channel: ");
  Serial.println(WiFi.channel());
  delay(2);

  byte mac[6];
  WiFi.macAddress(mac);
  sprintf(full_esp_now_ssid, "%s%02X%02X%02X%02X%02X%02X", "ESPNOW2", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.println("System ID's");
  Serial.println("Hostname " + String(gatewayName));
  Serial.println("ESP-NOW SSID " + String(full_esp_now_ssid));

  bool result = WiFi.softAP(full_esp_now_ssid, "12345678abcdefg");
  if (!result)
  {
    Serial.println("AP Config failed.");
    ESP.restart();
  }
  else
  {
    Serial.println("AP Config Success. Broadcasting with AP: " + String(WiFi.channel()));
  }

} // end initWifi
void handleTelnet()
{
  if (TelnetServer.hasClient())
  {
    if (!Telnet || !Telnet.connected())
    {
      if (Telnet)
        Telnet.stop();
      Telnet = TelnetServer.available();
    }
    else
    {
      TelnetServer.available().stop();
    }
  }
} // end handleTelnet
void hsWriteLog()
{
  // standard practisch to send a log entry to my domotica system
  // Send start message to homeseer log
  // Write an entry in the HomeSeer logfile
  HTTPClient http;
  Serial.println("Write log entry to HomeSeer");
  String url = "http://";

  // write startmessage into homeseer device 952
  url += homeseer;
  url += "/JSON?request=setdeviceproperty&ref=952&property=NewDevString&value=error%20";
  url += gatewayName;
  url += "%20with%20ipaddress%20";
  url += WiFi.localIP().toString();
  url += "%20has%20(re)started.";
  http.begin(url);
  int httpCode = http.GET();
  // httpCode will be negative on error
  if (httpCode > 0)
  {
    // HTTP header has been send and Server response header has been handled
    Serial.printf("[HTTP] GET. homeseer writelog success.. responce: %d\n", httpCode);
  }
  else
  {
    Serial.printf("[HTTP] GET. homeseer writelog failed.. error: %s\n", http.errorToString(httpCode).c_str());
    delay(30000);
    ESP.restart();
  }
  http.end();

  // Start event to write the startmessage in the homeseer log
  url = "http://";
  url += homeseer;
  url += "/JSON?request=runevent&group=System&name=WriteLog";
  http.begin(url);
  httpCode = http.GET();
  // httpCode will be negative on error
  if (httpCode > 0)
  {
    // HTTP header has been send and Server response header has been handled
    Serial.printf("[HTTP] GET. homeseer writelog success.. responce: %d\n", httpCode);
  }
  else
  {
    Serial.printf("[HTTP] GET. homeseer writelog failed.. error: %s\n", http.errorToString(httpCode).c_str());
    delay(30000);
    ESP.restart();
  }
  http.end();
}
void hsWriteLogError()
{ // Send error message to homeseer log
  HTTPClient http;
  Serial.println("Write log entry to HomeSeer");
  String url = "http://";
  url += homeseer;
  url += "/JSON?request=setdeviceproperty&ref=952&property=NewDevString&value=Error%20More%20then%2020%20sensors%20on%20espnowgw1%20with%20ipaddress%20";
  url += WiFi.localIP().toString();
  http.begin(url);
  int httpCode = http.GET();
  // httpCode will be negative on error
  if (httpCode > 0)
  {
    // HTTP header has been send and Server response header has been handled
    Serial.printf("[HTTP] GET. homeseer writelog success.. responce: %d\n", httpCode);
  }
  else
  {
    Serial.printf("[HTTP] GET. homeseer writelog failed.. error: %s\n", http.errorToString(httpCode).c_str());
    delay(30000);
    ESP.restart();
  }
  http.end();

  url = "http://";
  url += homeseer;
  url += "/JSON?request=runevent&group=System&name=WriteLog";
  http.begin(url);
  httpCode = http.GET();
  // httpCode will be negative on error
  if (httpCode > 0)
  {
    // HTTP header has been send and Server response header has been handled
    Serial.printf("[HTTP] GET. homeseer writelog success.. responce: %d\n", httpCode);
  }
  else
  {
    Serial.printf("[HTTP] GET. homeseer writelog failed.. error: %s\n", http.errorToString(httpCode).c_str());
    delay(30000);
    ESP.restart();
  }
  http.end();
}
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  /*
   Callback when data is sent from the Gateway to the sensor
   We will send a command to a sensor based on commands received by MQTT
   The sub will log if the command is received by the sensor

*/
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
           mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
//  Serial.print("ESP-Now Return Packet Sent to: ");
//  Serial.println(macStr);
//  Serial.print("ESP-Now Return Packet Send Status: ");
//  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
  if (status == ESP_NOW_SEND_SUCCESS) {
    for (int i = 0; i < numberOfSensors; i++) {
      if (strcmp(commandQueue[i].mac, macStr) == 0) { // we found the sensor
        commandQueue[i].jsonSentData[0] = NULL;
//        Serial.println("Command send and received. CommandQueue cleared");
      }
    }
  }
}
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *data, int data_len) {
/*
   Callback when data is received from the sensor
   1: Log that a packet is receive from mac_addr
   2: copy sensor data to secure it
   3: send data to sensor if a command is waiting the sensor (done in main loop)
   4: publish sensor data over mqtt (done in main loop)
*/
//  Serial.print("ESP-Now Packet Recv");
  if (!espCmdReceived) {
    snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
//    Serial.print("ESP-Now Packet Recv from: ");
//    Serial.println(macStr);
//    Serial.print("ESP-Now Packet Data: ");
    // save data so it can be processed with lower priority in the main loop
    for (int i = 0; i < data_len; i++) {
      bs[i] = data[i];
    }
    bs[data_len] = NULL;
//    Serial.println(bs);
    for (int i = 0; i < 6; ++i ) {
      peerAddr[i] = (uint8_t) mac_addr[i];
    }
    espCmdReceived = true;
  }
//  else {
//    Serial.println("Previous ESP command not processed yet. Skipping this one");
//  }
}
void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  if (mqttCmdReceived)
  {
    Serial.println("Previous MQTT command not processed yet. Skipping this one");
  }
  else
  {
    strcpy(mt, topic);
    strncpy(md, (char *)payload, length);
    md[length] = NULL;
    //    Serial.println("MQTT command received");
  }
  mqttCmdReceived = true;
}
void processMqttData()
{
  // data is in md[] and topic is in mt[]
  Telnet.print("Processing MQTT message [");
  Telnet.print(mt);
  Telnet.print("] ");
  Telnet.println(md);

  // parse topic in seperate items
  char items[5][20];
  byte _topicPos = 0;
  byte _itemPos = 0;
  byte _slash_count = 0;
  items[0][0] == NULL;
  while (1)
  {
    if (mt[_topicPos] == NULL)
    {
      items[_slash_count][_itemPos] = NULL;
      break;
    }
    if (mt[_topicPos] == 47)
    {
      if (_topicPos != 0)
      {
        items[_slash_count][_itemPos] = NULL;
        _slash_count++;
        _itemPos = 0;
        items[_slash_count][_itemPos] = NULL;
      }
    }
    else
    {
      items[_slash_count][_itemPos] = mt[_topicPos];
      _itemPos++;
    }
    _topicPos++;
  }

  boolean sensorFound = false;
  for (int i = 0; i < numberOfSensors; i++)
  {
    if (strcmp(commandQueue[i].mac, items[1]) == 0)
    { // found sensor in tabel
      char jsonTmp[250];
      StaticJsonBuffer<250> jsonBuffer;
      if (commandQueue[i].jsonSentData[0] == NULL)
      {
        JsonObject &root = jsonBuffer.createObject();
        root[items[2]] = md;
        root.printTo(commandQueue[i].jsonSentData, root.measureLength() + 1);
      }
      else
      {
        strcpy(jsonTmp, commandQueue[i].jsonSentData);
        //        Telnet.println(jsonTmp);
        JsonObject &root = jsonBuffer.parseObject(jsonTmp);
        if (!root.success())
        {
          Telnet.println("parseObject() failed");
        }
        root[items[2]] = md;
        root.printTo(commandQueue[i].jsonSentData, root.measureLength() + 1);
      }
      //      jsonBuffer.clear();
      if (strcmp(items[2], "updatefreq") == 0)
      {
        commandQueue[i].updateFreq = atoi(md);
      }
      sensorFound = true;
      Telnet.print("mac_addr found. Command: ");
      Telnet.print(commandQueue[i].jsonSentData);
      Telnet.println(" placed in queue");
      break;
    }
  }
  if (sensorFound == false)
  {
    // insert new sensor in tabel
    Telnet.println("New sensor received via MQTT");
    //   Telnet.print("mac_addr: ");
    strncpy(commandQueue[numberOfSensors].mac, items[1], 20);
    strcpy(commandQueue[numberOfSensors].lastTimeSeen, "Connection Lost");
    commandQueue[numberOfSensors].epochTime = 0;
    commandQueue[numberOfSensors].sensorEverSeen = false;
    commandQueue[numberOfSensors].jsonSentData[0] = NULL;
    strcat(commandQueue[numberOfSensors].jsonSentData, "{\"");
    strcat(commandQueue[numberOfSensors].jsonSentData, items[2]);
    strcat(commandQueue[numberOfSensors].jsonSentData, "\":\"");
    strcat(commandQueue[numberOfSensors].jsonSentData, md);
    strcat(commandQueue[numberOfSensors].jsonSentData, "\"}");
    if (strcmp(items[2], "updatefreq") == 0)
    {
      commandQueue[numberOfSensors].updateFreq = atoi(md);
    }
    // add it to the sensor peer list
    slave.channel = 0;
    slave.encrypt = 0;
    // convert StringMac[12] into bytemac[6]
    int ii = 0;
    for (int i = 0; i < 12; i += 2)
    {
      slave.peer_addr[ii] = getVal(items[1][i + 1]) + (getVal(items[1][i]) << 4);
      //      Telnet.print(slave.peer_addr[ii]);
      ii += 1;
    }

    //    Telnet.println();
    const esp_now_peer_info_t *peer = &slave;
    esp_err_t addStatus = esp_now_add_peer(peer);
    if (addStatus == ESP_OK)
    {
      // Pair success
      Telnet.print("Command: ");
      Telnet.println(commandQueue[numberOfSensors].jsonSentData);
    }
    else if (addStatus == ESP_ERR_ESPNOW_NOT_INIT)
      Telnet.println("ESPNOW Not Init");
    else if (addStatus == ESP_ERR_ESPNOW_ARG)
      Telnet.println("Invalid Argument");
    else if (addStatus == ESP_ERR_ESPNOW_FULL)
      Telnet.println("Peer list full");
    else if (addStatus == ESP_ERR_ESPNOW_NO_MEM)
      Telnet.println("Out of memory");
    else if (addStatus == ESP_ERR_ESPNOW_EXIST)
      Telnet.println("Peer Exists");
    else
      Telnet.println("Not sure what happened");
    numberOfSensors++;
    if (numberOfSensors > 20)
    {
      Telnet.println("Error: more then 20 sensors on the gateway!");
      hsWriteLogError();
      numberOfSensors--;
    }
  }
}
byte getVal(char c)
{
  if (c >= '0' && c <= '9')
    return (byte)(c - '0');
  else
    return (byte)(c - 'A' + 10);
}
void reconnect()
{
  // Loop until we're reconnected
  while (!client.connected())
  {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    char gatewayStatusTopic[100] = {'\0'};
    strcat(gatewayStatusTopic, gatewayName);
    strcat(gatewayStatusTopic, "/status");
    if (client.connect(gatewayName, gatewayStatusTopic, MQTTQOS0, true, "Connection Lost"))
    {
      Serial.println("connected");
      // Once connected, publish an announcement...
      // ... and resubscribe
      client.subscribe("espnowgw/#");
    }
    else
    {
      Serial.print("failed to connect to MQTT, rc=");
      Serial.print(client.state());
      ++failedMqttConnect;
      if (failedMqttConnect > 500)
      {
        Serial.println(" Just reboot we lost the mqtt connection so badly");
        delay(1000);
        ESP.restart();
      }
      delay(1000);
    }
  }
  if (failedMqttConnect > 400)
    failedMqttConnect = 0;
}
void initOTA()
{
  ArduinoOTA.setHostname(gatewayName);
  ArduinoOTA
      .onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
          type = "sketch";
        else // U_SPIFFS
          type = "filesystem";

        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
        Serial.println("Start updating " + type);
      })
      .onEnd([]() {
        Serial.println("\nEnd");
      })
      .onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
      })
      .onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR)
          Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR)
          Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR)
          Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR)
          Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR)
          Serial.println("End Failed");
      });

  ArduinoOTA.begin();
}

// WebServer routines
void initWebserver()
{
  server.on("/", handleRoot);
  server.on("/restart", restart);
  server.on("/updateOTA", updateOTA);
  server.on("/cancelOTA", cancelUpdateOTA);
//  server.on("/sendDataInQueue", sendDataInQueue);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
  delay(10);
}
void updateOTA()
{
  server.send(200, "text/plain", "Going into OTA programming mode");
  flagOTA = true;
}
void cancelUpdateOTA()
{
  server.send(200, "text/plain", "Canceling OTA programming mode");
  flagOTA = false;
}
void sendDataInQueue()
{
  server.send(200, "text/plain", "Sending data in Queue to Sensors");
  sendSensorDataInQueue();
}
void handleRoot()
{
  String message = "Hello from ";
  message += gatewayName;
  message += "\n\nProgram: ESPNow32_Gateway_V2.1\n";
  message += "Telnet to this device for debug output\n";
  message += "\nCurrent time: ";
  message += timeClient.getFormattedTime();
  message += "\nUsages:\n";
  message += "/                  - This messages\n";
  message += "/updateOTA         - Put device in OTA programming mode\n";
  message += "/cancelOTA         - Cancel OTA programming mode\n";
  message += "/restart           - Restarts the ESPNow gateway\n\n";
  message += "Programming mode: ";
  //  message += flagOTA;
  message += "\nNumber of sensors: ";
  message += numberOfSensors;
  message += "\n\n";
  message += "N) MAC           UpdateFreq LastTimeSeen  JsonSentData\n";
  for (int i = 0; i < numberOfSensors; i++)
  {
    message += i;
    message += ") ";
    message += commandQueue[i].mac;
    message += "  ";
    message += commandQueue[i].updateFreq;
    message += "         ";
    message += commandQueue[i].lastTimeSeen;
    message += "  ";
    message += commandQueue[i].jsonSentData;
    message += "\n";
  }

  message += "\nGateway Macadres: ";
  message +=  WiFi.macAddress();
  message += "\nWiFi channel: ";
  message += WiFi.channel();
  message += "\nRSSI: ";
  message += WiFi.RSSI();
  message += "\nOTA flag: ";
  message += flagOTA;
  message += "\nFree memory: ";
  message += ESP.getFreeHeap();
  message += "\nfailedMqttReconnect: ";
  message += failedMqttConnect;
  message += "\n\n";
  server.send(200, "text/plain", message);
}
void restart()
{
  server.send(200, "text/plain", "OK restarting");
  delay(2000);
  ESP.restart();
}
void handleNotFound()
{
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++)
  {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
}

