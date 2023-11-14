#include "auxiliars.hpp"
#include "env.hpp"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <SPIFFS.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <iostream>
#include <sstream>
#include <string>

/// loop ///
StaticJsonDocument<2000> wsDoc;
DeserializationError wsMsgDesErr;

StaticJsonDocument<512> reqRewriteDoc;
DeserializationError reqRewriteStrDesErr;
String createdAtToSince;

/// WSE ///
File reqWSEFile;
String reqWSEStr;
String reqPrefix = "[\"REQ\",\"query:data\",";
String reqStrToSend;

unsigned int stringToUnsignedInt(const std::string &str) {
  std::istringstream iss(str);
  unsigned int result;
  iss >> result;
  return result;
}

/////////////////////////////////////// NTP ///////////////////////////////////
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");
///////////////////////////////////// end NTP //////////////////////////////////

//////////////////////////////////// SOCKET ///////////////////////////////////
WebSocketsClient webSocket; // declare instance of websocket

String wsMsg;
bool newMsg = false;

void webSocketEvent(WStype_t type, uint8_t *strload, size_t length);
////////////////////////////////// end SOCKET /////////////////////////////////

void setup() {
  Serial.begin(9600);
  pinMode(BUILTIN_LED, OUTPUT);

  //// Conection to WiFi ////
  Serial.print("Conecting to WiFi");
  WiFi.begin(ENV_SSID, ENV_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    blinkOnboardLed();
    delay(700);
  }
  Serial.println("\nWiFi Connected!");

  //// Start NTP ////
  timeClient.begin();
  timeClient.update();
  Serial.printf("Start time: %lu\n", timeClient.getEpochTime());

  //// Start SPIFFS ////
  if (!SPIFFS.begin()) {
    Serial.println("error: not initialized SPIFFS");
    return;
  } else {
    Serial.println("Initialized SPIFFS");
  }

  //// REQ ////
  File reqFile = SPIFFS.open("/req-template.json", "r");
  String reqStr;

  if (reqFile) {
    Serial.println("File \"/req-template.json\" opened");
    reqStr = reqFile.readString();
    if (reqStr == "") {
      Serial.println("error: empty file \"/req-template.json\"");
      reqFile.close();
    } else {
      Serial.println("File read successfully, content:\n" + reqStr);
      reqFile.close();
    }
  } else {
    Serial.println("error: not opened file \"/req-template.json\"");
  }

  // Deserialize reqStr
  StaticJsonDocument<512> reqDoc;
  DeserializationError reqStrDesErr = deserializeJson(reqDoc, reqStr);

  if (reqStrDesErr) {
    Serial.print("error: not deserialize reJson: ");
    Serial.println(reqStrDesErr.f_str());
  }

  if (ENV_KINDS != NULL) {
    JsonArray kinds = reqDoc.createNestedArray("kinds");
    for (int i = 0; i < sizeof(ENV_KINDS) / sizeof(ENV_KINDS[0]); i++) {
      kinds.add(ENV_KINDS[i]);
    }
  }
  if (ENV_PUBKEYS != NULL) {
    JsonArray pubkeys = reqDoc.createNestedArray("#p");
    for (int i = 0; i < sizeof(ENV_PUBKEYS) / sizeof(ENV_PUBKEYS[0]); i++) {
      pubkeys.add(ENV_PUBKEYS[i]);
    }
  }
  reqDoc["since"] = (unsigned int)(timeClient.getEpochTime());
  reqDoc["until"] = ENV_UNTIL;
  Serial.println("Variables loaded to reqDoc");

  // Serialize newReqStr
  String newReqStr;
  serializeJson(reqDoc, newReqStr);
  Serial.println("New content of newReqStr: " + newReqStr);

  // Save data
  File newReqFile = SPIFFS.open("/req-data.json", "w");
  if (!newReqFile) {
    Serial.println("error: not opened file \"/req-data.json\"");
    return;
  } else {
    Serial.println("File \"/req-data.json\" opened");
    newReqFile.print(newReqStr);
    Serial.println("New content of file \"/req-data.json\": " + newReqStr);
    newReqFile.close();
  }

  // Conection to websocket ////
  Serial.print("Conceting to websocket");

  webSocket.beginSSL(ENV_WSURL, 443);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(1000);
  while (!webSocket.isConnected()) {
    Serial.print(".");
    blinkOnboardLed();
    webSocket.loop();
  }
  Serial.println("Conceted to websocket!");
}

void loop() {
  while (newMsg == false) {
    //// Check WiFi ////
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected");
      Serial.print("Reconecting to WiFi");
      while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        blinkOnboardLed();
        delay(700);
      }
      Serial.print("\nWiFi reconnected!");
    }

    //// Check websocket ////
    webSocket.loop();
  }

  //// Recive messagge ////
  /// Print data of event
  Serial.println("--------------------------------------------");
  Serial.println("==> New message:\n" + wsMsg);

  /// Deserialize wsMsg
  wsMsgDesErr = deserializeJson(wsDoc, wsMsg);

  if (wsMsgDesErr) {
    Serial.print("error: not deserialize wsMsg: ");
    Serial.println(wsMsgDesErr.f_str());
  }
  Serial.println("----------------------");

  /// Print data of event
  if (wsDoc[0].as<String>() == "EOSE") {
    Serial.println("EOSE: end of stream");
  } else {
    Serial.println("EVENT CONTENT");
    Serial.println("id: " + wsDoc[2]["id"].as<String>());
    Serial.println("kind: " + wsDoc[2]["kind"].as<String>());
    Serial.println("pubkey: " + wsDoc[2]["pubkey"].as<String>());
    createdAtToSince = wsDoc[2]["created_at"].as<String>();
    Serial.println("created_at: " + createdAtToSince);
    Serial.println("content: " + wsDoc[2]["content"].as<String>());
    for (size_t i = 0; i < 4; i++) {
      Serial.println("tags: " + wsDoc[2]["tags"][i].as<String>());
    }
    Serial.println("sig: " + wsDoc[2]["sig"].as<String>());
    Serial.println("----------------------");

    //// rewrite since in /req-data.json ////
    // Open /req-data.json
    File reqRewriteFile = SPIFFS.open("/req-data.json", "r");
    String reqRewriteStr;

    if (reqRewriteFile) {
      Serial.println("File \"/req-data.json\" opened");
      reqRewriteStr = reqRewriteFile.readString();
      if (reqRewriteStr == "") {
        Serial.println("error: empty file \"/req-data.json\"");
        reqRewriteFile.close();
        return;
      } else {
        Serial.println("File read successfully, content:\n" + reqRewriteStr);
        reqRewriteFile.close();
      }
    } else {
      Serial.println("error: not opened file \"/req-data.json\"");
    }

    // Deserialize reqRewriteStr
    reqRewriteStrDesErr = deserializeJson(reqRewriteDoc, reqRewriteStr);

    if (reqRewriteStrDesErr) {
      Serial.print("error: not deserialize reqRewriteStr");
      Serial.println(reqRewriteStrDesErr.f_str());
    }

    // Rewrite since
    reqRewriteDoc["since"] = createdAtToSince.toInt() + 10;
    Serial.print("since rewrite successfully with: ");
    Serial.println(createdAtToSince.toInt() + 10);

    // Serialize newReqRewriteStr
    String newReqRewriteStr;
    serializeJson(reqRewriteDoc, newReqRewriteStr);
    Serial.println("New content of newReqRewriteStr: " + newReqRewriteStr);

    // Save data
    reqRewriteFile = SPIFFS.open("/req-data.json", "w");
    if (!reqRewriteFile) {
      Serial.println("error: not opened file \"/req-data.json\"");
      return;
    } else {
      Serial.println("File \"/req-data.json\" opened");
      reqRewriteFile.print(newReqRewriteStr);
      Serial.println("New content of file \"/req-data.json\": " + newReqRewriteStr);
      reqRewriteFile.close();
    }
  }
  Serial.println("==> End of message");
  Serial.println("--------------------------------------------");

  newMsg = false;
}

////////////////// WEBSOCKET ///////////////////

void webSocketEvent(WStype_t type, uint8_t *strload, size_t length) {
  switch (type) {
  case WStype_DISCONNECTED:
    Serial.println("[WS] Disconnected!");
    break;
  case WStype_CONNECTED:
    Serial.println("\n[WS] Connected");
    /// Obtain reqWSEStr ///
    reqWSEFile = SPIFFS.open("/req-data.json", "r");

    if (reqWSEFile) {
      Serial.println("File \"/req-data.json\" opened");
      reqWSEStr = reqWSEFile.readString();
      if (reqWSEStr == "") {
        Serial.println("error: empty file \"/req-data.json\"");
        reqWSEFile.close();
        break;
      } else {
        Serial.println("File read successfully, content:\n" + reqWSEStr);
        reqWSEFile.close();
      }
    } else {
      Serial.println("error: not opened file \"/req-data.json\"");
      break;
    }

    reqStrToSend = reqPrefix + reqWSEStr + "]";
    Serial.println("Sending message to server: " + reqStrToSend);
    webSocket.sendTXT(reqStrToSend); // send message to server when Connected
    break;
  case WStype_TEXT:
    wsMsg = (char *)strload;
    Serial.println("\nReceived data from socket");
    newMsg = true;
    break;
  case WStype_ERROR:
  case WStype_FRAGMENT_TEXT_START:
  case WStype_FRAGMENT_BIN_START:
  case WStype_FRAGMENT:
  case WStype_FRAGMENT_FIN:
    break;
  }
}