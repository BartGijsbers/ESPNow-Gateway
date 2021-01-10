// standard practisch to send a log entry to my domotica system

void hsWriteLog()
{ // Send start message to homeseer log
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
