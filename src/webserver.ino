

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
  message += "\n\nProgram: ESPNow32_Gateway_V2.0\n";
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
