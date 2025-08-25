#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ArduinoJson.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"

// WiFi and MQTT configuration
WiFiClient espClient;
PubSubClient client(espClient);
DHT dht(DHTPIN, DHTTYPE);

// FreeRTOS handles
QueueHandle_t dataQueue;
SemaphoreHandle_t xMutex;
EventGroupHandle_t mqttEventGroup;
const int CONNECTED_BIT = BIT0;

typedef struct {
    unsigned long num;
    float temp;
    float humi;
    float timestamp;
} SensorData;

unsigned long totalLatency = 0;
unsigned long messageReceived = 0;
unsigned long messageSent = 0;

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

void callback(char* topic, byte* payload, unsigned int length) {
    if (length >= 256) length = 255; // 限制最大長度，避免 buffer overflow
    char message[256];
    memcpy(message, payload, length);
    message[length] = '\0';

    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, message);
    if (!error) {
        unsigned long sentTime = doc["timestamp"];
        unsigned long now = esp_timer_get_time() / 1000;
        unsigned long latency = now - sentTime;
        // Serial.printf("Received message #%lu, Latency: %lu ms\n", doc["num"].as<unsigned long>(), latency);
        if(messageReceived == 0 && doc["num"].as<unsigned long>() != 1) {
            return;
        }
        xSemaphoreTake(xMutex, portMAX_DELAY);
        totalLatency += latency;
        messageReceived++;
        xSemaphoreGive(xMutex);
    } else {
        Serial.println("Failed to parse JSON.");
    }
}

void SensorTask(void *pvParameters) {
    while(1) {
        SensorData data;
        data.num = ++messageSent; // Increment message number
        data.temp = dht.readTemperature();
        data.humi = dht.readHumidity();
        data.timestamp = esp_timer_get_time() / 1000;

        if(!isnan(data.temp)) {
            xQueueSend(dataQueue, &data, pdMS_TO_TICKS(100));
        }

        vTaskDelay(pdMS_TO_TICKS(10000)); // 2 seconds
    }
}

void MQTTTask(void *pvParameters) {
    while(1) {
        xEventGroupWaitBits(mqttEventGroup, CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        SensorData data;
        if(xQueueReceive(dataQueue, &data, portMAX_DELAY)) {
            StaticJsonDocument<256> doc;
            doc["num"] = data.num;
            doc["temperature"] = data.temp;
            doc["humidity"] = data.humi;
            doc["timestamp"] = data.timestamp;

            char buffer[256];
            size_t n = serializeJson(doc, buffer);
            client.publish(MQTT_SENSOR_TOPIC, buffer, true);
            // Serial.printf("Send message #%lu\n", data.num);
        }
    }
}

void LatencyTask(void *pvParameters) {
    while(1) {
        unsigned long now = esp_timer_get_time() / 1000;
        if (messageReceived > 0) {
            xSemaphoreTake(xMutex, portMAX_DELAY);
            unsigned long averageLatency = totalLatency / messageReceived;
            xSemaphoreGive(xMutex);
            Serial.printf("Average latency: %lu ms\n", averageLatency);
        } else {
            Serial.println("No messages sent yet.");
        }
        vTaskDelay(pdMS_TO_TICKS(50000));
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
    dht.begin();
    client.setServer(MQTT_SERVER, MQTT_PORT);
    client.setCallback(callback);

    dataQueue = xQueueCreate(5, sizeof(SensorData));
    xMutex = xSemaphoreCreateMutex();
    mqttEventGroup = xEventGroupCreate();
    xTaskCreatePinnedToCore(MQTTLoopTask, "MQTTLoopTask", 8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(SensorTask, "SensorTask", 2048, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(MQTTTask, "MQTTTask", 8192, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(LatencyTask, "LatencyTask", 2048, NULL, 1, NULL, 1);
}

void loop() {
    // RTOS handle everything, so the loop can be empty
}