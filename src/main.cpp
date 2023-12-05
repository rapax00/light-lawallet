#include "auxiliars.hpp"
#include "env.hpp"
#include <Adafruit_Thermal.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <BluetoothSerial.h>
#include <HTTPClient.h>
#include <NTPClient.h>
#include <SPIFFS.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <iostream>
#include <sstream>
#include <string>

//// loop ////
StaticJsonDocument<2000> wsDoc;
DeserializationError wsMsgDesErr;

StaticJsonDocument<512> reqRewriteDoc;
DeserializationError reqRewriteStrDesErr;
String createdAtToSince;

//// WS ////
File reqWSFile;
String reqWSStr;
String reqPrefix = "[\"REQ\",\"query:data\",";
String reqStrToSend;

/////////////////////////////////////// NTP ///////////////////////////////////
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");
///////////////////////////////////// end NTP //////////////////////////////////

//////////////////////////////////// SOCKET ///////////////////////////////////
WebSocketsClient webSocket; // declare instance of websocket

String wsMsg;
bool newMsg = false;

String reqStr;

void webSocketEvent(WStype_t type, uint8_t *strload, size_t length);
////////////////////////////////// end SOCKET /////////////////////////////////

/////////////////////////////// HTTP //////////////////////////////////////////

HTTPClient http;

//////////////////////////////// end HTTP /////////////////////////////////////

/////////////////////////////// BLUETOOTH /////////////////////////////////////
BluetoothSerial SerialBT;
//////////////////////////////// end BLUETOOTH ///////////////////////////////

/////////////////////////////// PRINTER ///////////////////////////////////////
Adafruit_Thermal printer(&SerialBT);
//////////////////////////////// end PRINTER ///////////////////////////////////

void setup() {
  Serial.begin(9600);
  pinMode(BUILTIN_LED, OUTPUT);

  //// Conection to WiFi ////
  // Serial.printf("=== WiFi ===\n");
  // Serial.printf("Conecting to WiFi");
  WiFi.begin(ENV_SSID, ENV_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    // Serial.printf(".");
    blinkOnboardLed();
    delay(700);
  }
  // Serial.printf("\nWiFi Connected!\n\n\n");

  //// Start NTP ////
  // Serial.printf("=== TIME ===\n");
  timeClient.begin();
  timeClient.update();
  // Serial.printf("Start time: %lu\n\n\n", timeClient.getEpochTime());

  //// Start SPIFFS ////
  // Serial.printf("=== SPIFFS ===\n");
  if (SPIFFS.begin()) {
    // Serial.printf("Initialized SPIFFS\n\n\n");
  } else {
    // Serial.printf("error: not initialized SPIFFS\n\n\n");
    return;
  }

  //// PUBKEY ////
  // Serial.printf("=== HTTP ===\n");
  http.begin("https://lawallet.ar/.well-known/nostr.json?name=" + String(ENV_LNADDRESS)); // Specify request destination

  int httpCode = http.GET(); // get request

  String httppayload;
  if (httpCode) {
    httppayload = http.getString(); // get response
    // Serial.printf("httpCode: %s\n", httppayload.c_str());
  } else {
    // Serial.printf("error: not get pubkey\n\n\n");
    return;
  }

  http.end(); // Close connection

  // Deserialize httppayload
  StaticJsonDocument<1024> httpDoc;
  DeserializationError htttpStrDesErr = deserializeJson(httpDoc, httppayload);

  if (htttpStrDesErr) {
    // Serial.printf("error: not deserialize httppayload: %s\n\n\n", htttpStrDesErr.f_str());
    return;
  } else {
    // Serial.printf("htttpayload deserialized successfully\n");
  }

  String pubkey = (httpDoc["names"][ENV_LNADDRESS]).as<String>();
  // Serial.printf("pubkey: %s\n\n\n", pubkey.c_str());

  //// REQ ////
  // Serial.printf("=== REQ ===\n");
  File reqFile = SPIFFS.open("/req-template.json", "r");
  String reqStr;

  if (reqFile) {
    // Serial.printf("File \"/req-template.json\" opened\n");
    reqStr = reqFile.readString();
    if (reqStr == "") {
      // Serial.printf("error: empty file \"/req-template.json\"\n\n\n");
      reqFile.close();
      return;
    } else {
      // Serial.printf("File read successfully, content:\n%s\n", reqStr.c_str());
      reqFile.close();
    }
  } else {
    // Serial.printf("error: not opened file \"/req-template.json\"\n");
    return;
  }

  // Deserialize reqStr
  StaticJsonDocument<512> reqDoc;
  DeserializationError reqStrDesErr = deserializeJson(reqDoc, reqStr);

  if (reqStrDesErr) {
    // Serial.printf("error: not deserialize reqStr: %s\n\n\n", reqStrDesErr.f_str());
    return;
  }

  if (ENV_KINDS != NULL) {
    JsonArray kinds = reqDoc.createNestedArray("kinds");
    for (int i = 0; i < sizeof(ENV_KINDS) / sizeof(ENV_KINDS[0]); i++) {
      kinds.add(ENV_KINDS[i]);
    }
  }
  if (pubkey != NULL) {
    JsonArray pubkeys = reqDoc.createNestedArray("#p");
    pubkeys.add(pubkey);
  }
  reqDoc["since"] = (unsigned int)(timeClient.getEpochTime());
  reqDoc["until"] = ENV_UNTIL;
  JsonArray tags = reqDoc.createNestedArray("#t");
  tags.add("inbound-transaction-ok");
  tags.add("internal-transaction-ok");

  // Serial.printf("Variables loaded to reqDoc\n");

  // Serialize newReqStr
  String newReqStr;
  serializeJson(reqDoc, newReqStr);
  // Serial.printf("New content of newReqStr: %s\n", newReqStr.c_str());

  // Save data
  File newReqFile = SPIFFS.open("/req-data.json", "w");
  if (!newReqFile) {
    // Serial.printf("error: not opened file \"/req-data.json\"\n\n\n");
    return;
  } else {
    // Serial.printf("File \"/req-data.json\" opened\n");
    newReqFile.print(newReqStr);
    // Serial.printf("New content of file \"/req-data.json\": %s\n\n\n", newReqStr.c_str());
    newReqFile.close();
  }

  //// Conection to websocket ////
  // Serial.printf("=== WebSocket ===\n");
  // Serial.printf("Conceting to websocket");

  webSocket.beginSSL(ENV_WSURL, 443);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(1000);
  while (!webSocket.isConnected()) {
    Serial.print(".");
    blinkOnboardLed();
    webSocket.loop();
  }
  // Serial.printf("Conceted to websocket!\n\n\n");
}

void loop() {
  while (newMsg == false) {
    //// Check WiFi ////
    if (WiFi.status() != WL_CONNECTED) {
      // Serial.printf("WiFi disconnected\n");
      // Serial.printf("Reconecting to WiFi");
      while (WiFi.status() != WL_CONNECTED) {
        // Serial.printf(".");
        blinkOnboardLed();
        delay(700);
      }
      // Serial.printf("\nWiFi reconnected!\n");
    }

    //// Check websocket ////
    webSocket.loop();
  }

  //// Recive messagge ////
  // Print data of event
  // Serial.printf("!--------------------------------------------\n");
  // Serial.printf("==> New message:\n%s\n", wsMsg.c_str());

  // Deserialize wsMsg
  wsMsgDesErr = deserializeJson(wsDoc, wsMsg);

  if (wsMsgDesErr) {
    // Serial.printf("error: not deserialize wsMsg: %s\n", wsMsgDesErr.f_str());
  }
  // Serial.printf("----------------------\n");

  // Print data of event
  if (wsDoc[0].as<String>() == "EOSE") {
    // Serial.printf("EOSE: end of stream\n");
  } else {
    // Serial.printf("EVENT CONTENT\n");
    // Serial.printf("id: %s\n", (wsDoc[2]["id"].as<String>()).c_str());
    // Serial.printf("kind: %s\n", (wsDoc[2]["kind"].as<String>()).c_str());
    // Serial.printf("pubkey: %s\n", (wsDoc[2]["pubkey"].as<String>()).c_str());
    createdAtToSince = wsDoc[2]["created_at"].as<String>();
    // Serial.printf("created_at: %s\n", createdAtToSince.c_str());
    // Serial.printf("content: %s\n", (wsDoc[2]["content"].as<String>()).c_str());
    for (size_t i = 0; i < (wsDoc[2]["tags"]).size(); i++) {
      // Serial.printf("tags: %s\n", (wsDoc[2]["tags"][i].as<String>()).c_str());
    }
    // Serial.printf("sig: %s\n", (wsDoc[2]["sig"].as<String>()).c_str());
    // Serial.printf("----------------------\n");

    //// rewrite since in /req-data.json ////
    // Open /req-data.json
    File reqRewriteFile = SPIFFS.open("/req-data.json", "r");
    String reqRewriteStr;

    if (reqRewriteFile) {
      // Serial.printf("File \"/req-data.json\" opened\n");
      reqRewriteStr = reqRewriteFile.readString();
      if (reqRewriteStr == "") {
        // Serial.printf("error: empty file \"/req-data.json\"\n\n");
        reqRewriteFile.close();
        return;
      } else {
        // Serial.printf("File read successfully, content:\n%s\n", reqRewriteStr.c_str());
        reqRewriteFile.close();
      }
    } else {
      // Serial.printf("error: not opened file \"/req-data.json\"\n\n");
      return;
    }

    // Deserialize reqRewriteStr
    reqRewriteStrDesErr = deserializeJson(reqRewriteDoc, reqRewriteStr);

    if (reqRewriteStrDesErr) {
      // Serial.printf("error: not deserialize reqRewriteStr %s\n\n", reqRewriteStrDesErr.f_str());
      return;
    }

    // Rewrite since
    reqRewriteDoc["since"] = createdAtToSince.toInt() + 10;
    // Serial.printf("since rewrite successfully with: %d\n", createdAtToSince.toInt() + 10);

    // Serialize newReqRewriteStr
    String newReqRewriteStr;
    serializeJson(reqRewriteDoc, newReqRewriteStr);
    // Serial.printf("New content of newReqRewriteStr: %s\n", newReqRewriteStr.c_str());

    // Save data
    reqRewriteFile = SPIFFS.open("/req-data.json", "w");
    if (!reqRewriteFile) {
      // Serial.printf("error: not opened file \"/req-data.json\"\n\n");
      return;
    } else {
      // Serial.printf("File \"/req-data.json\" opened\n");
      reqRewriteFile.print(newReqRewriteStr);
      // Serial.printf("New content of file \"/req-data.json\": \n%s\n", newReqRewriteStr.c_str());
      reqRewriteFile.close();
    }
  }
  // Serial.printf("==> End of message\n");
  // Serial.printf("!--------------------------------------------\n");

  newMsg = false;

  //// Hadware Notice ////

  JsonArray tags = wsDoc[2]["tags"].as<JsonArray>();

  size_t i;
  for (i = 0; i < (wsDoc[2]["tags"]).size(); i++) {
    if (tags[i].as<String>().substring(2, 3) == "t") {
      // Serial.printf("tag: %s\n", (tags[i].as<String>()).c_str());
      break;
    }
  }

  StaticJsonDocument<128> tagTDoc;
  DeserializationError tagTDesErr = deserializeJson(tagTDoc, tags[i].as<String>());
  // DeserializationError tagTDesErr = deserializeJson(tagsDoc, tagsDoc["t"].as<String>());

  if (tagTDesErr) {
    // Serial.printf("error: not deserialize content: %s %s\n", tagTDesErr.f_str());
  }

  String tagT = tagTDoc[1].as<String>();
  int tagTLength = tagT.length();
  if (tagT.substring(tagTLength - 2, tagTLength) == "ok") {

    StaticJsonDocument<128> contentDoc;
    DeserializationError contentDesErr = deserializeJson(contentDoc, wsDoc[2]["content"].as<String>());
    DeserializationError tokensDesErr = deserializeJson(contentDoc, contentDoc["tokens"].as<String>());

    if (contentDesErr && tokensDesErr) {
      // Serial.printf("error: not deserialize content: %s %s\n", contentDesErr.f_str(), tokensDesErr.f_str());
    } else {
      // Serial.printf("content deserialized successfully\n");
    }

    int amount = contentDoc["BTC"].as<int>();

    amount /= 1000;
    // Serial.printf("1. Amount: %d sats\n", amount);

    printer.wake();
    printer.println("Amount recived: " + String(amount) + " sats");
    printer.feed(2);
    printer.sleep();

    while (true) {
      amount /= 10;
      digitalWrite(BUILTIN_LED, HIGH);
      delay(250);
      digitalWrite(BUILTIN_LED, LOW);
      delay(250);
      if (amount == 0) {
        break;
      }
    }
  }
}

////////////////// WEBSOCKET ///////////////////

void webSocketEvent(WStype_t type, uint8_t *strload, size_t length) {
  switch (type) {
  case WStype_DISCONNECTED:
    // Serial.printf("[WS] Disconnected!\n");
    break;
  case WStype_CONNECTED:
    // Serial.printf("\n[WS] Connected\n");
    /// Obtain reqWSStr ///
    reqWSFile = SPIFFS.open("/req-data.json", "r");

    if (reqWSFile) {
      // Serial.printf("File \"/req-data.json\" opened\n");
      reqWSStr = reqWSFile.readString();
      if (reqWSStr == "") {
        // Serial.printf("error: empty file \"/req-data.json\"\n\n");
        reqWSFile.close();
        break;
      } else {
        // Serial.printf("File read successfully, content: \n%s\n", reqWSStr.c_str());
        reqWSFile.close();
      }
    } else {
      // Serial.printf("error: not opened file \"/req-data.json\"\n\n");
      break;
    }

    reqStrToSend = reqPrefix + reqWSStr + "]";
    // Serial.printf("Sending message to server: \n%s\n", reqStrToSend.c_str());
    webSocket.sendTXT(reqStrToSend); // send message to server when Connected
    break;
  case WStype_TEXT:
    wsMsg = (char *)strload;
    // Serial.printf("\n=== Received data from socket ===\n");
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