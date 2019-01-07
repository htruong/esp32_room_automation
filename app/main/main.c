#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event_loop.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "driver/gpio.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include <dht.h>

static const char *TAG = "MQTT_BEDROOMAUTOMATION";

static EventGroupHandle_t wifi_event_group;
const static int CONNECTED_BIT = BIT0;

#define RELAY_GPIO CONFIG_RELAY_GPIO

//int dht_sensor_last_pull = 0;
int16_t dht_sensor_last_temp = 0;
int16_t dht_sensor_last_humid = 0;

static const dht_sensor_type_t sensor_type = DHT_TYPE_DHT22;
static const gpio_num_t dht_gpio = CONFIG_DHT_GPIO;

esp_mqtt_client_handle_t client = NULL;
int8_t client_ready = 0;

void dht_update_sensor(void *pvParameters)
{
    int16_t temperature = 0;
    int16_t humidity = 0;

    // DHT sensors that come mounted on a PCB generally have
    // pull-up resistors on the data pin.  It is recommended
    // to provide an external pull-up resistor otherwise...

    gpio_set_pull_mode(dht_gpio, GPIO_PULLUP_ONLY);

    while (1)
    {
        if (dht_read_data(sensor_type, dht_gpio, &humidity, &temperature) == ESP_OK)
            printf("Updating Humidity: %d%% Temp: %dC\n", humidity / 10, temperature / 10);
	    dht_sensor_last_temp = temperature;
	    dht_sensor_last_humid = humidity;
	    if (client_ready) {
		    int msg_id;
		    char buf[10];

		    sprintf(buf, "%d", temperature/10);
		    msg_id = esp_mqtt_client_publish(client, CONFIG_MQTT_TOPIC_PREFIX "/getTemp", buf, 0, 1, 1);
		    ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

		    sprintf(buf, "%d", humidity/10);
		    msg_id = esp_mqtt_client_publish(client, CONFIG_MQTT_TOPIC_PREFIX "/getHumid", buf, 0, 1, 1);
		    ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
	    }
        else
            printf("Could not read data from sensor\n");

        vTaskDelay(60000 / portTICK_PERIOD_MS);
    }
}


static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    // your_context_t *context = event->context;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            msg_id = esp_mqtt_client_publish(client, CONFIG_MQTT_TOPIC_PREFIX "/getOn", "0", 0, 1, 1);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_publish(client, CONFIG_MQTT_TOPIC_PREFIX "/getOnline", "1", 0, 1, 1);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_subscribe(client, CONFIG_MQTT_TOPIC_PREFIX "/setOn", 0);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

	    client_ready = 1;
            break;
        case MQTT_EVENT_DISCONNECTED:
	    client_ready = 0;
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
           break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");

	    int desired_state = 0;

            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);

	    if (strncmp(event->topic, CONFIG_MQTT_TOPIC_PREFIX "/setOn", event->topic_len) == 0) {
		if (strncmp(event->data, "1", 1) == 0) {
		    desired_state = 1;
		}
		gpio_set_level(RELAY_GPIO, desired_state);
		msg_id = esp_mqtt_client_publish(client, CONFIG_MQTT_TOPIC_PREFIX "/getOn", desired_state==1?"1":"0", 0, 1, 1);
                ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
	    }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;
    }
    return ESP_OK;
}

static esp_err_t wifi_event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
        case SYSTEM_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);

            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void wifi_init(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_init(wifi_event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_LOGI(TAG, "start the WIFI SSID:[%s]", CONFIG_WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Waiting for wifi");
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
}

static void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = CONFIG_BROKER_URL,
        .event_handle = mqtt_event_handler,
        // .user_context = (void *)your_context
    };

#if CONFIG_BROKER_URL_FROM_STDIN
    char line[128];

    if (strcmp(mqtt_cfg.uri, "FROM_STDIN") == 0) {
        int count = 0;
        printf("Please enter url of mqtt broker\n");
        while (count < 128) {
            int c = fgetc(stdin);
            if (c == '\n') {
                line[count] = '\0';
                break;
            } else if (c > 0 && c < 127) {
                line[count] = c;
                ++count;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        mqtt_cfg.uri = line;
        printf("Broker url: %s\n", line);
    } else {
        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
        abort();
    }
#endif /* CONFIG_BROKER_URL_FROM_STDIN */

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(client);
}


static void configure_gpios()
{
    gpio_pad_select_gpio(RELAY_GPIO);
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(RELAY_GPIO, 0);
}

void app_main()
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    configure_gpios();

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    nvs_flash_init();
    wifi_init();
    xTaskCreate(dht_update_sensor, "dht_update_sensor", configMINIMAL_STACK_SIZE * 3, NULL, 5, NULL);
    mqtt_app_start();
}
