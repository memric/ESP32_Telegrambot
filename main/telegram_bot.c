/*
 * telegram_bot.c
 *
 *  Created on: 26.08.2022
 *      Author: chudnikov
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "cJSON/cJSON.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifndef USE_TLS_BUNDLE
#define USE_TLS_BUNDLE		0
#endif

#define WEB_SERVER 			"api.telegram.org"
#define BOT_TOKEN			"523164356:AAFz9slk922jd0usbGzoNbYR4N8f3RFVr44"
#define WEB_URL 			"https://api.telegram.org/bot"

#define HTTP_METHOD_GET		"GET"
#define HTTP_METHOD_POST	"POST"

static const char *TAG = "TELEGRAM";
static TaskHandle_t telebot_task = NULL;

#if !USE_TLS_BUNDLE
extern const uint8_t telegram_cert_pem_start[] asm("_binary_telegram_cert_pem_start");
extern const uint8_t telegram_cert_pem_end[]   asm("_binary_telegram_cert_pem_end");
#endif
char url[256];
char buf[512];
char resp[512] = {0};

static int32_t TeleBot_Get_Request(const char *http_mthd, const char *t_mthd, char *resp, uint32_t resp_len);
static void TeleBot_Task(void *arg);

void TeleBot_Init(void)
{
	xTaskCreate(TeleBot_Task, "Telegram Bot", 1024*6, NULL, 3, &telebot_task);
}

static int32_t TeleBot_Get_Request(const char *http_mthd, const char *t_mthd, char *resp, uint32_t resp_len)
{
    int ret, len;
    int32_t ret_len = 0;
//    char *pcopy = resp;

#if USE_TLS_BUNDLE
    ESP_LOGI(TAG, "https_request using crt bundle");
    esp_tls_cfg_t cfg = {
    		.crt_bundle_attach = esp_crt_bundle_attach,
    };
#else
    ESP_LOGI(TAG, "https_request using cacert_buf");
    esp_tls_cfg_t cfg = {
    		.cacert_buf = (const unsigned char *) telegram_cert_pem_start,
			.cacert_bytes = telegram_cert_pem_end - telegram_cert_pem_start,
    };
#endif

    /*Compose url*/
    len = snprintf(url, sizeof(url), ""WEB_URL"%s/%s", BOT_TOKEN, t_mthd);

    if (len < 0) return -1;

    ESP_LOGD(TAG, "HTTPS url: %s", url);

    struct esp_tls *tls = esp_tls_conn_http_new(url, &cfg);

    if (tls != NULL)
    {
        ESP_LOGI(TAG, "Connection established...");

        /*Compose request*/
        len = snprintf(buf, sizeof(buf), "%s /bot%s/%s HTTP/1.1\r\n"
        		"Host: "WEB_SERVER"\r\n"
//                "User-Agent: esp-idf/1.0 esp32\r\n"
				"Connection: close\r\n"
				"\r\n",
				http_mthd, BOT_TOKEN, t_mthd);

        if (len < 0) return -1;

        ESP_LOGD(TAG, "HTTP Request: %s", buf);

        size_t written_bytes = 0;
        do {
        	/*write request*/
        	ret = esp_tls_conn_write(tls, buf + written_bytes, len - written_bytes);

        	if (ret >= 0)
        	{
        		ESP_LOGI(TAG, "%d bytes written", ret);
        		written_bytes += ret;
        	}
        	else if (ret != ESP_TLS_ERR_SSL_WANT_READ  && ret != ESP_TLS_ERR_SSL_WANT_WRITE)
        	{
        		ESP_LOGE(TAG, "esp_tls_conn_write  returned: [0x%02X](%s)", ret, esp_err_to_name(ret));
        		break;
        	}
        } while (written_bytes < len);

        if (written_bytes == len)
        {
        	ESP_LOGI(TAG, "Reading HTTP response...");

        	do {
        		len = sizeof(buf);
        		bzero(buf, sizeof(buf));
        		ret = esp_tls_conn_read(tls, (char *) buf, len);

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

        		/*JSON beginning searching*/
        		char *pch = strstr(buf, "\r\n{");
        		if (pch != NULL)
        		{
        			ret_len = 0;
        			len -= (pch - buf);
        		}
        		else
        		{
        			pch = buf;
        		}
        		/*Copy response*/
        		while ((len > 0) && (ret_len < resp_len))
        		{
        			*resp++ = *pch++;
        			ret_len++;
        			len--;
        		}

        	} while (ret != 0);
        }
    }
    else
    {
        ESP_LOGE(TAG, "Connection failed...");
    }

    esp_tls_conn_delete(tls);

    return ret_len;
}

static void TeleBot_GetUpdates(void)
{
	int32_t rx_len = TeleBot_Get_Request("GET", "getUpdates", resp, sizeof(resp));

	if (rx_len > 0)
	{
		ESP_LOGD(TAG, "Resp: %s", resp);

		cJSON *json = cJSON_ParseWithLength(resp, sizeof(resp));

		if (json != NULL)
		{
			cJSON *result = cJSON_GetObjectItemCaseSensitive(json, "result");
			cJSON *res_item;
			cJSON_ArrayForEach(res_item, result)
			{
				cJSON *message = cJSON_GetObjectItemCaseSensitive(res_item, "message");
				cJSON *text = cJSON_GetObjectItemCaseSensitive(message, "text");

				if (text != NULL)
				{
					cJSON *chat = cJSON_GetObjectItemCaseSensitive(message, "chat");
					cJSON *chat_id = cJSON_GetObjectItemCaseSensitive(chat, "id");

					ESP_LOGI(TAG, "Message from %d: %s", chat_id->valueint, text->valuestring);
				}
			}
		}
	}
}

/**
 * Main task
 */
static void TeleBot_Task(void *arg)
{
	(void) arg;

	while (1)
	{
		TeleBot_GetUpdates();
		vTaskDelay(1000 / portTICK_PERIOD_MS);
	}
}
