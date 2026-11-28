// Live stream from OpenF1's MQTT broker (ESP-IDF esp-mqtt, TLS, the OAuth
// token as the password). Each message is one row of the matching REST
// endpoint; rows are collected per topic and handed over as JSON arrays, so
// the model sees the same shape whether a row was pushed or polled.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum MqttTopic : uint8_t {
  MT_POSITION, MT_INTERVALS, MT_RACE_CONTROL, MT_LAPS, MT_PIT, MT_STINTS, MT_WEATHER,
  MT_OVERTAKES, MT_LOCATION, MT_CAR_DATA, MT_TEAM_RADIO, MT_COUNT
};

// Called with one batch: a JSON array of rows of that topic. May run on the
// MQTT task or on the caller of mqttDrain(), never on both at once.
typedef void (*MqttSink)(MqttTopic topic, const char *json, size_t len);

void mqttBegin(MqttSink sink);
// Start (or restart) the connection with this token. Does nothing while a
// connection is up or still being attempted.
void mqttConnect(const char *token);
void mqttStop();
bool mqttUp();                  // connected and subscribed
bool mqttFailed();              // the last attempt ended without getting there
// How long it has not been up, through however many attempts (0 while up,
// 0xFFFFFFFF when stopped).
uint32_t mqttDownMs();
uint32_t mqttRowCount();        // rows received since boot
// Hand over whatever has been collected. Call often from the network task;
// the MQTT task does it too if that task is busy for a while.
void mqttDrain();
// While on, the MQTT task only collects rows; nothing reaches the sink until
// the caller turns it off and drains. For reading by REST first what the
// stream will not send (a session's history, the rows of a break): pushed
// rows must not be applied ahead of older ones still on their way.
void mqttHold(bool on);
void mqttPublish(const char *topic, const char *payload);   // bench clock only
