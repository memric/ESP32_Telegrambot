#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "esp_tls.h"
#include "esp_crt_bundle.h"

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT	BIT0
#define WIFI_FAIL_BIT		BIT1

#define WEB_SERVER 			"api.telegram.org"
#define BOT_TOKEN			"523164356:AAFz9slk922jd0usbGzoNbYR4N8f3RFVr44"
#define WEB_URL 			"https://api.telegram.org/bot"BOT_TOKEN"/getMe"

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

static const char *TAG = "MAIN";

static const char REQUEST[] = "GET /bot"BOT_TOKEN"/getMe HTTP/1.1\r\n"
                             "Host: "WEB_SERVER"\r\n"
//                             "User-Agent: esp-idf/1.0 esp32\r\n"
//							 "Connection: keep-alive\r\n"
                             "\r\n";

extern const uint8_t telegram_cert_pem_start[] asm("_binary_telegram_cert_pem_start");
extern const uint8_t telegram_cert_pem_end[]   asm("_binary_telegram_cert_pem_end");

esp_err_t event_handler(void *ctx, system_event_t *event)
{
    return ESP_OK;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
	{
		esp_wifi_connect();
	}
	else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
	{
		if (s_retry_num < 5/*ESP_MAXIMUM_RETRY*/)
		{
			esp_wifi_connect();
			s_retry_num++;
			ESP_LOGI(TAG, "retry to connect to the AP");
		}
		else
		{
			xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
		}
		ESP_LOGI(TAG,"connect to the AP fail");
	}
	else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
	{
		ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
		ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
		s_retry_num = 0;
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
	}
}

static void https_get_request(esp_tls_cfg_t cfg)
{
    char buf[512];
    int ret, len;

    struct esp_tls *tls = esp_tls_conn_http_new(WEB_URL, &cfg);

    if (tls != NULL) {
        ESP_LOGI(TAG, "Connection established...");
    } else {
        ESP_LOGE(TAG, "Connection failed...");
        goto exit;
    }

    ESP_LOGI(TAG, "Request size %d", sizeof(REQUEST));
    ESP_LOGI(TAG, "Request: %s", REQUEST);

    size_t written_bytes = 0;
    do {
        ret = esp_tls_conn_write(tls,
                                 REQUEST + written_bytes,
                                 sizeof(REQUEST) - 1 - written_bytes);
        if (ret >= 0) {
            ESP_LOGI(TAG, "%d bytes written", ret);
            written_bytes += ret;
        } else if (ret != ESP_TLS_ERR_SSL_WANT_READ  && ret != ESP_TLS_ERR_SSL_WANT_WRITE) {
            ESP_LOGE(TAG, "esp_tls_conn_write  returned: [0x%02X](%s)", ret, esp_err_to_name(ret));
            goto exit;
        }
    } while (written_bytes < sizeof(REQUEST) - 1);

    ESP_LOGI(TAG, "Reading HTTP response...");


    do {
        len = sizeof(buf) - 1;
        bzero(buf, sizeof(buf));
        ret = esp_tls_conn_read(tls, (char *)buf, len);

        if (ret == ESP_TLS_ERR_SSL_WANT_WRITE  || ret == ESP_TLS_ERR_SSL_WANT_READ) {
            continue;
        }

        if (ret < 0) {
            ESP_LOGE(TAG, "esp_tls_conn_read  returned [-0x%02X](%s)", -ret, esp_err_to_name(ret));
            break;
        }

        if (ret == 0) {
            ESP_LOGI(TAG, "connection closed");
            break;
        }

        len = ret;
        ESP_LOGD(TAG, "%d bytes read", len);
        /* Print response directly to stdout as it is read */
        for (int i = 0; i < len; i++) {
            putchar(buf[i]);
        }
        putchar('\n'); // JSON output doesn't have a newline at end
    } while (1);

exit:
    esp_tls_conn_delete(tls);
//    for (int countdown = 10; countdown >= 0; countdown--) {
//        ESP_LOGI(TAG, "%d...", countdown);
//        vTaskDelay(1000 / portTICK_PERIOD_MS);
//    }
}

static void https_get_request_using_crt_bundle(void)
{
    ESP_LOGI(TAG, "https_request using crt bundle");
    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    https_get_request(cfg);
}

static void https_get_request_using_cacert_buf(void)
{
    ESP_LOGI(TAG, "https_request using cacert_buf");
    esp_tls_cfg_t cfg = {
        .cacert_buf = (const unsigned char *) telegram_cert_pem_start,
        .cacert_bytes = telegram_cert_pem_end - telegram_cert_pem_start,
    };
    https_get_request(cfg);
}

void app_main(void)
{
	/*Initialize NVS*/
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	ESP_ERROR_CHECK(esp_netif_init());

	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

    s_wifi_event_group = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
    		ESP_EVENT_ANY_ID,
			&wifi_event_handler,
			NULL,
			&instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
    		IP_EVENT_STA_GOT_IP,
			&wifi_event_handler,
			NULL,
			&instance_got_ip));

    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    wifi_config_t sta_config = {
        .sta = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASSWORD,
            .bssid_set = false
        }
    };
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &sta_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
    ESP_ERROR_CHECK( esp_wifi_connect() );

    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
    int level = 0;
    while (true) {

    	/*Check WiFi events*/
    	if (s_wifi_event_group)
    	{
    		EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
    				WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
					pdTRUE,
					pdFALSE,
					300 / portTICK_PERIOD_MS);

    		/* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
    		 * happened. */
    		if (bits & WIFI_CONNECTED_BIT)
    		{
    			ESP_LOGI(TAG, "Connected to AP SSID:%s", CONFIG_ESP_WIFI_SSID);

//    			https_get_request_using_crt_bundle();
    			https_get_request_using_cacert_buf();
    		}
    		else if (bits & WIFI_FAIL_BIT)
    		{
    			ESP_LOGI(TAG, "Failed to connect to SSID:%s", CONFIG_ESP_WIFI_PASSWORD);
    		}
    	}

        gpio_set_level(GPIO_NUM_2, level);
        level = !level;
    }
}

