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
#include <inttypes.h>

#ifndef USE_TLS_BUNDLE
#define USE_TLS_BUNDLE		0
#endif

#define WEB_SERVER 			"api.telegram.org"
#define WEB_URL 			"https://api.telegram.org/bot"
#define BOT_TOKEN			CONFIG_TG_BOT_TOKEN

#define UPDATE_TIMEOUT		5

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

static cJSON *mainMarkup = NULL;

static int32_t TeleBot_Http_Request(const char *http_mthd, const char *t_mthd,
		char *req, uint32_t req_len,
		char *resp, uint32_t resp_len);
static void TeleBot_Task(void *arg);
int32_t TeleBot_SendMessage(uint32_t chat_id, const char *msg, cJSON *markup);
void TeleBot_MessageCallback(uint32_t chat_id, const char *msg);

/**
 * @brief	Bot initialization
 *
 */
void TeleBot_Init(void)
{
	xTaskCreate(TeleBot_Task, "Telegram Bot", 1024*6, NULL, 3, &telebot_task);
}

/**
 * @brief 	Connects to Telegram bot server, sends http request and
 * 			gets response
 *
 * @param http_mthd	HTTP method GET or POST
 * @param t_mthd	Telegram method
 * @param req		Request string (JSON)
 * @param req_len	Request length
 * @param resp		Pointer to buffer for response from server
 * @param resp_len	Response lengthp
 * @return
 */
static int32_t TeleBot_Http_Request(const char *http_mthd, const char *t_mthd,
		char *req, uint32_t req_len,
		char *resp, uint32_t resp_len)
{
	int32_t ret, len;
	int32_t http_req_len = 0;
    int32_t ret_len = 0;

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

    /*Set connection*/
    esp_tls_t *tls = esp_tls_init();
    if (!tls)
    {
        ESP_LOGE(TAG, "Failed to allocate esp_tls handle!");
        return -1;
    }

    if (esp_tls_conn_http_new_sync(url, &cfg, tls) == 1)
    {
        ESP_LOGI(TAG, "Connection established...");

        /*Compose request header*/
        len = snprintf(buf, sizeof(buf), "%s /bot%s/%s HTTP/1.1\r\n"
        		"Host: "WEB_SERVER"\r\n"
                /*"User-Agent: esp-idf/1.0 esp32\r\n"*/
				"Connection: close\r\n",
				http_mthd, BOT_TOKEN, t_mthd);

        if (len > 0)
        {
        	http_req_len += len;

        	if ((req != NULL) && (req_len < (sizeof(buf) - http_req_len)))
        	{
        		/*Append request string*/
        		len = snprintf(&buf[http_req_len], sizeof(buf) - len,
        				"Content-Type: application/json\r\n"
        				"Content-Length: %"PRIu32"\r\n\r\n", req_len);

        		if (len > 0)
        		{
        			http_req_len += len;

        			len = snprintf(&buf[http_req_len], sizeof(buf) - http_req_len, "%s", req);

        			if (len > 0) { http_req_len += len; }
        			else { http_req_len = 0; }
        		}
        		else
        		{
        			http_req_len = 0;
        		}
        	}
        	else
        	{
        		/*Append \r\n for GET request*/
        		len = snprintf(&buf[http_req_len], sizeof(buf) - http_req_len, "\r\n");

        		if (len > 0) { http_req_len += len; }
        		else { http_req_len = 0; }
        	}

        	ESP_LOGI(TAG, "HTTP Request: %s", buf);
        }

        if (http_req_len == 0)
        {
        	ESP_LOGE(TAG, "Request composing error");
        }
        else
        {
        	size_t written_bytes = 0;

        	/*write request*/
        	do {
        		ret = esp_tls_conn_write(tls, buf + written_bytes, http_req_len - written_bytes);

        		if (ret >= 0)
        		{
        			ESP_LOGI(TAG, "%"PRIi32" bytes written", ret);
        			written_bytes += ret;
        		}
        		else if (ret != ESP_TLS_ERR_SSL_WANT_READ  && ret != ESP_TLS_ERR_SSL_WANT_WRITE)
        		{
        			ESP_LOGE(TAG, "esp_tls_conn_write returned: [0x%02"PRIx32"](%s)", ret, esp_err_to_name(ret));
        			break;
        		}
        	} while (written_bytes < http_req_len);

        	/*Read response*/
        	if (written_bytes == http_req_len)
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
        				ESP_LOGE(TAG, "esp_tls_conn_read returned [-0x%02"PRIx32"](%s)", -ret, esp_err_to_name(ret));
        				break;
        			}

        			if (ret == 0) {
        				ESP_LOGI(TAG, "connection closed");
        				break;
        			}

        			len = ret;
        			ESP_LOGD(TAG, "%"PRIi32" bytes read", len);
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
    }
    else
    {
        ESP_LOGE(TAG, "Connection failed...");
    }

    /*Delete connection*/
    esp_tls_conn_destroy(tls);

    return ret_len;
}

/**
 * getUpdates method
 */
static int32_t TeleBot_GetUpdates(int32_t *id)
{
	int32_t retval = -1;

	/*Construct JSON method object*/
	cJSON *message = cJSON_CreateObject();

	if (message != NULL)
	{
		cJSON *msg_timeout = cJSON_CreateNumber(UPDATE_TIMEOUT);
		cJSON *msg_offset = cJSON_CreateNumber(*id);

		if (msg_timeout != NULL && msg_offset != NULL)
		{
			cJSON_AddItemToObject(message, "timeout", msg_timeout);
			cJSON_AddItemToObject(message, "offset", msg_offset);

			char *req = cJSON_PrintUnformatted(message);

			retval = TeleBot_Http_Request("POST", "getUpdates", req, strlen(req), resp, sizeof(resp));

			if (retval > 0)
			{
				ESP_LOGD(TAG, "Resp: %s", resp);

				cJSON *json = cJSON_ParseWithLength(resp, sizeof(resp));

				if (json != NULL)
				{
					cJSON *result = cJSON_GetObjectItemCaseSensitive(json, "result");
					cJSON *res_item;
					cJSON_ArrayForEach(res_item, result)
					{
						/*Get update id*/
						cJSON *upd_id = cJSON_GetObjectItemCaseSensitive(res_item, "update_id");
						if (upd_id != NULL && upd_id->valueint >= *id)
						{
							/*Recalculate offset*/
							*id = upd_id->valueint + 1;
						}

						cJSON *message = cJSON_GetObjectItemCaseSensitive(res_item, "message");
						cJSON *text = cJSON_GetObjectItemCaseSensitive(message, "text");

						if (text != NULL)
						{
							cJSON *chat = cJSON_GetObjectItemCaseSensitive(message, "chat");
							cJSON *chat_id = cJSON_GetObjectItemCaseSensitive(chat, "id");

							TeleBot_MessageCallback((uint32_t) chat_id->valueint, text->valuestring);
						}
					}

					cJSON_Delete(json);
				}
			}

			free(req);
		}

		cJSON_Delete(message);
	}

	return retval;
}

/**
 * Sends message
 */
int32_t TeleBot_SendMessage(uint32_t chat_id, const char *msg, cJSON *markup)
{
	int32_t retval = -1;

	/*Construct JSON method object*/
	cJSON *message = cJSON_CreateObjectReference(NULL);

	if (message != NULL)
	{
		cJSON *msg_chat_id = cJSON_CreateNumber(chat_id);
		cJSON *msg_text = cJSON_CreateString(msg);

		if (msg_chat_id != NULL && msg_text != NULL)
		{
			cJSON_AddItemToObject(message, "chat_id", msg_chat_id);
			cJSON_AddItemToObject(message, "text", msg_text);

			if (markup != NULL)
			{
				cJSON_AddItemToObject(message, "reply_markup", markup);
			}

			char *req = cJSON_PrintUnformatted(message);

			ESP_LOGD(TAG, "sendMessage: %s", req);

			retval = TeleBot_Http_Request("POST", "sendMessage", req, strlen(req), resp, sizeof(resp));

			free(req);
		}

		cJSON_Delete(message);
	}

	return retval;
}

/**
 * Main task
 */
static void TeleBot_Task(void *arg)
{
	(void) arg;
	int32_t id = -1;
	int32_t resp_len;

	/*Create markup*/
	mainMarkup = cJSON_CreateObject();

	if (mainMarkup != NULL)
	{
		cJSON *btn1 = cJSON_CreateString("Btn1");
		cJSON *btn2 = cJSON_CreateString("Btn2");
		cJSON *btn3 = cJSON_CreateString("Btn3");

		cJSON *btns = cJSON_CreateArray();
		cJSON *row1 = cJSON_CreateArray();
		cJSON *row2 = cJSON_CreateArray();

		if (btn2 != NULL && btn1 != NULL && btn3 != NULL &&
				btns != NULL && row1 != NULL && row2 != NULL)
		{
			cJSON_AddItemToArray(row1, btn1);
			cJSON_AddItemToArray(row1, btn2);
			cJSON_AddItemToArray(row2, btn3);
			cJSON_AddItemToArray(btns, row1);
			cJSON_AddItemToArray(btns, row2);

			cJSON_AddItemToObject(mainMarkup, "keyboard", btns);
		}
	}

	while (1)
	{
		/*Get updates*/
		resp_len = TeleBot_GetUpdates(&id);

		if (resp_len > 0 && id != -1)
		{
			memset(resp, 0, resp_len);
		}

		vTaskDelay(1000 / portTICK_PERIOD_MS);
	}
}

/**
 * Message callback
 */
void TeleBot_MessageCallback(uint32_t chat_id, const char *msg)
{
	if (chat_id != 243661148) return;

	ESP_LOGI(TAG, "Message from %"PRIu32": %s", chat_id, msg);

	/*Message echo*/
	TeleBot_SendMessage(chat_id, msg, mainMarkup);
}
