/*
   Callback when data is sent from the Gateway to the sensor
   We will send a command to a sensor based on commands received by MQTT
   The sub will log if the command is received by the sensor

*/

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
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


/*
   Callback when data is received from the sensor
   1: Log that a packet is receive from mac_addr
   2: copy sensor data to secure it
   3: send data to sensor if a command is waiting the sensor (done in main loop)
   4: publish sensor data over mqtt (done in main loop)
*/

void OnDataRecv(const uint8_t *mac_addr, const uint8_t *data, int data_len) {
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

