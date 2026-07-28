// mqtt.h — Home Assistant integration via MQTT discovery (PubSubClient).
//
// URI format in settings: mqtt://[user[:pass]@]host[:port]   (default port 1883)
// On connect, publishes HA discovery configs (retained) for:
//   sensor.airradar_in_range, sensor.airradar_heard, sensor.airradar_nearest,
//   sensor.airradar_nearest_km, sensor.airradar_feed_rate, sensor.airradar_source,
//   binary_sensor.airradar_emergency
// plus availability topic airradar/status (online/offline LWT).
// State topics under airradar/state/* published every AR_MQTT_PUB_MS.
// Remember: PubSubClient default buffer is 256 B — setBufferSize(1024).
#pragma once
#include "../core/state.h"

void mqttBegin();                   // parse URI, set LWT; no-op if disabled
void mqttLoop(uint32_t nowMs);      // maintain connection + periodic publish
void mqttRestart();                 // after settings change
