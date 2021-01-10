
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

// data is in md[] and topic is in mt[]
void processMqttData()
{
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

int failedMqttConnect = 0;

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
