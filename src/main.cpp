#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include <vector>

// WiFi and MQTT configuration
WiFiClient espClient;
PubSubClient client(espClient);

// DS18B20 configuration
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// FreeRTOS handles
QueueHandle_t dataQueue;
SemaphoreHandle_t xMutex;
EventGroupHandle_t mqttEventGroup;
const int CONNECTED_BIT = BIT0;

typedef struct {
    uint64_t id;
    char addr[17]; // 16 chars + null terminator
    float temp;
    float timestamp;
} SensorData;

void MQTTLoopTask(void *pvParameters) {
    while (1) {
        if(!client.connected()) {
            xEventGroupClearBits(mqttEventGroup, CONNECTED_BIT);
            while (!client.connected()) {
                if(client.connect(CLIENTID)) {
                    client.subscribe(MQTT_SENSOR_TOPIC);
                    xEventGroupSetBits(mqttEventGroup, CONNECTED_BIT); // contect
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        client.loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

uint64_t addressToUint64(const uint8_t addr[8]) {
    uint64_t id = 0;
    for (int i = 2; i < 8; i++) {
        id |= ((uint64_t)addr[i] << (i * 8));
    }
    return id;
}
void addressToChar(DeviceAddress deviceAddress, char* buffer) {
  for (uint8_t i = 0; i < 8; i++) {
    sprintf(buffer + i * 2, "%02X", deviceAddress[i]);
  }
  buffer[16] = '\0';
}
void SensorTask(void *pvParameters) {
    while(1) {
        std::vector<SensorData> dataVec;
        DeviceAddress devAddr;
        oneWire.reset_search(); // Reset the search state to find all devices
        int count = 0;
        while(oneWire.search(devAddr)) {
            if(devAddr[0] == 0x28) { // Check if the device is a DS18B20
                SensorData data;
                sensors.requestTemperatures();
                float temp = sensors.getTempC(devAddr);
                if(temp == DEVICE_DISCONNECTED_C) data.temp = NAN;
                else data.temp = temp;

                data.id = count++;
                addressToChar(devAddr, (char*)&data.addr);
                dataVec.push_back(data);

                Serial.print("Device Address: ");
                Serial.print(data.addr);
                Serial.print(" Temp: ");
                Serial.println(data.temp);
            }
        }

        for(auto& data : dataVec) {
            float ts_ms = (float) esp_timer_get_time() / 1000;
            data.timestamp = ts_ms / 1000.0f; // convert to seconds
            xQueueSend(dataQueue, &data, pdMS_TO_TICKS(100));
        }

        // sensors.requestTemperatures();
        // int deviceCount = sensors.getDeviceCount();
        // SensorData data[deviceCount];
        // std::vector<SensorData> dataVec(deviceCount);

        // Serial.println(deviceCount);

        // for(int i = 0; i < deviceCount; i++) {
        //     float temp = sensors.getTempCByIndex(i);
        //     data[i].id = i;
        //     data[i].timestamp = esp_timer_get_time() / 1000;

        //     if(temp == DEVICE_DISCONNECTED_C) data[i].temp = NAN;
        //     else data[i].temp = temp;
        // }

        // for(int i = 0; i < deviceCount; i++) {
        //     xQueueSend(dataQueue, &data[i], pdMS_TO_TICKS(100));
        // }

        vTaskDelay(pdMS_TO_TICKS(TEMP_PUBLISH_INTERVAL_MS));
    }
}

void MQTTTask(void *pvParameters) {
    while(1) {
        xEventGroupWaitBits(mqttEventGroup, CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        SensorData data;
        if(xQueueReceive(dataQueue, &data, portMAX_DELAY)) {
            StaticJsonDocument<256> doc;
            doc["id"] = data.id;
            doc["address"] = data.addr;
            doc["temperature"] = data.temp;
            doc["timestamp"] = data.timestamp;

            char buffer[256];
            size_t n = serializeJson(doc, buffer);
            client.publish(MQTT_SENSOR_TOPIC, buffer, true);
            // Serial.printf("Send message #%lu\n", data.num);
        }
    }
}

void setup_wifi() {
    delay(10);
    WiFi.begin(SSID, PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
    }
}

void setup() {
    Serial.begin(115200);
    setup_wifi();
    sensors.begin();
    client.setServer(MQTT_SERVER, MQTT_PORT);

    dataQueue = xQueueCreate(DS_DEV_COUNT, sizeof(SensorData));
    xMutex = xSemaphoreCreateMutex();
    mqttEventGroup = xEventGroupCreate();
    xTaskCreatePinnedToCore(MQTTLoopTask, "MQTTLoopTask", 8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(SensorTask, "SensorTask", 2048, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(MQTTTask, "MQTTTask", 8192, NULL, 2, NULL, 1);
}

void loop() {
    // RTOS handle everything, so the loop can be empty
}