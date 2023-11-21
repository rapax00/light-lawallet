// // #include "handlejson.hpp"
// #include <Arduino.h>
// #include <ArduinoJson.h>
// #include <SPIFFS.h>

// bool initializeSPIFFS() {
//   /// Start SPIFFS ///
//   if (!SPIFFS.begin()) {
//     Serial.println("error: not initialized SPIFFS");
//     return false;
//   } else {
//     Serial.println("Initialized SPIFFS");
//   }

//   return true;
// }

// String openAndReadFile(String path) {
//   String data;
//   File file;

//   file = SPIFFS.open(path, "r");
//   if (file) {
//     Serial.println("File opened");
//     data = file.readString();
//     if (data == "" || data == "[]") {
//       Serial.println("File is empty");
//       file.close();
//       return "";
//     } else {
//       Serial.println("File read successfully");
//     }
//   } else {
//     Serial.println("File not opened");
//     return "";
//   }
//   file.close();

//   return data;
// }

// bool deserialize(StaticJsonDocument *doc, String data) {
//   DeserializationError error = deserializeJson(doc, data);
//   if (error) {
//     Serial.println("error: deserialize json");
//     return doc;
//   } else {
//     Serial.println("json deserialized");
//   }

//   return true;
// }

// bool loadENVData() {}