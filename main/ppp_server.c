/* $Id$ */
/* PPP_SERVER Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <regex.h>

#include "driver/uart.h"
#include "soc/uart_struct.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "lwip/opt.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/lwip_napt.h"
#include "lwip/etharp.h"
#include "netif/ppp/ppp.h"
#include "netif/ppp/pppapi.h"
#include "netif/ppp/ppp_impl.h"
#include "netif/ethernet.h"

#include "esp_console.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_netif_net_stack.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"

#include "mqtt_client.h"
#include "cli_server.h"
#include "cli_client.h"
#include "cmd_iperf.h"
#include "cmd_ping.h"
#include "cmd_system.h"
#include "cmd_wifi.h"
#include "ppp_server.h"

static const char *TAG = "ppp_server";
static QueueHandle_t uart_event_queue;
static int current_phase = NETIF_PPP_PHASE_DEAD;
static int last_phase = NETIF_PPP_PHASE_DEAD;
static ppp_link_config_t config;
volatile bool sta_connected;

static const char *ppp_phase_str[13] = {
#define PPP_PHASE_STR(id)   [id] = #id
    PPP_PHASE_STR(PPP_PHASE_DEAD),         /* 0  */
    PPP_PHASE_STR(PPP_PHASE_MASTER),       /* 1  */
    PPP_PHASE_STR(PPP_PHASE_HOLDOFF),      /* 2  */
    PPP_PHASE_STR(PPP_PHASE_INITIALIZE),   /* 3  */
    PPP_PHASE_STR(PPP_PHASE_SERIALCONN),   /* 4  */
    PPP_PHASE_STR(PPP_PHASE_DORMANT),      /* 5  */
    PPP_PHASE_STR(PPP_PHASE_ESTABLISH),    /* 6  */
    PPP_PHASE_STR(PPP_PHASE_AUTHENTICATE), /* 7  */
    PPP_PHASE_STR(PPP_PHASE_CALLBACK),     /* 8  */
    PPP_PHASE_STR(PPP_PHASE_NETWORK),      /* 9  */
    PPP_PHASE_STR(PPP_PHASE_RUNNING),      /* 10 */
    PPP_PHASE_STR(PPP_PHASE_TERMINATE),    /* 11 */
    PPP_PHASE_STR(PPP_PHASE_DISCONNECT),   /* 12 */
#undef PPP_PHASE_STR
};

static wifi_config_t wifi_config = { 0 };
static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ppp_netif = NULL;

static esp_err_t on_ppp_transmit(void *h, void *buffer, size_t len)
{
    size_t free_size = 0;
    ESP_ERROR_CHECK(uart_get_tx_buffer_free_size(config.uart, &free_size));

    if (unlikely(free_size < len)) {
        ESP_LOGW(TAG, "Uart TX buffer full. free_size: %d len: %d", free_size, len);
        return ESP_FAIL;
    }
    int written = uart_write_bytes(config.uart, buffer, len);
    if (unlikely(len != written)) {
        ESP_LOGE(TAG, "Failed to write bytes. bytes: %d free: %d written: %d", len, free_size, written);
        abort();
    }
    return ESP_OK;
}

static esp_netif_t* new_ppp_server(void)
{
    const esp_netif_driver_ifconfig_t driver_ifconfig = {
        .driver_free_rx_buffer = NULL,
        .transmit = on_ppp_transmit,
    };
    // enable both events, so we could notify the modem layer if an error occurred/state changed
    const esp_netif_ppp_config_t ppp_config = { .ppp_error_event_enabled = true, .ppp_phase_event_enabled = true };
    const esp_netif_config_t ppp_cfg = ESP_NETIF_DEFAULT_PPP();

    esp_netif_t *_netif = esp_netif_new(&ppp_cfg);
    assert(_netif);

    ESP_ERROR_CHECK(esp_netif_set_driver_config(_netif, &driver_ifconfig));
    ESP_ERROR_CHECK(esp_netif_ppp_set_params(_netif, &ppp_config));

#ifdef CONFIG_PPP_SERVER_SUPPORT
    if (config.type == PPP_LINK_SERVER) {
        config.ppp_server.auth_req = !0;
        ESP_ERROR_CHECK(esp_netif_ppp_start_server(_netif, config.ppp_server.localaddr, config.ppp_server.remoteaddr, config.ppp_server.dnsaddr1,
                                                   config.ppp_server.dnsaddr2, config.ppp_server.login, config.ppp_server.password,
                                                   config.ppp_server.auth_req));
    }
#endif

    return _netif;
}

static void del_ppp_server(esp_netif_t *_netif)
{
    if (NULL != _netif) {
        esp_netif_destroy(_netif);
    }
}

bool wifi_connect(void)
{
    if (NULL != sta_netif) {
        ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
        ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );

        sta_connected = 0;
        if (ESP_OK == esp_wifi_connect()) {

           int cntr = 0;

           do {
               vTaskDelay(50 / portTICK_PERIOD_MS);
               if (sta_connected || cntr++ >= 200) {
                   break;
               }
               taskYIELD();
           } while (1);
        }
    }
    return sta_connected;
}

bool wifi_start(void)
{
    if (NULL == sta_netif) {
        const wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
        sta_netif = esp_netif_create_default_wifi_sta();
        assert(sta_netif);

        esp_netif_set_hostname(sta_netif, "esp_modem");
        ESP_ERROR_CHECK( esp_wifi_init(&wifi_cfg) );
        ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
        ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_NULL) );
        ESP_ERROR_CHECK( esp_wifi_start() );
    }
    return (sta_netif != NULL);
}

bool wifi_stop(void)
{
    if (NULL != sta_netif) {
        wifi_mode_t mode;
        esp_wifi_get_mode(&mode);

        if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
            esp_err_t ret = esp_wifi_disconnect();
            if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_INIT) {
                ESP_LOGW(TAG, "Disconnect returned: %s", esp_err_to_name(ret));
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        esp_wifi_stop();
        esp_wifi_deinit();

        esp_netif_destroy(sta_netif);
    }

    sta_netif = NULL;
    return true;
}

static void ppp_task_thread(void *param)
{
    ppp_netif = NULL;
    int answer_modem = 1;
    while (1) {
        uart_event_t event;

        if (xQueueReceive(uart_event_queue, &event, pdMS_TO_TICKS(100))) {

            switch (event.type) {
                case UART_DATA: {
                    while (true) {
                        char buffer[512];
                        size_t length = 0;

                        uart_get_buffered_data_len(config.uart, &length);

                        if (!length) {
                            break;
                        }

                        memset(buffer, 0, sizeof(buffer));
                        length = MIN(sizeof(buffer), length);
                        size_t read_length = uart_read_bytes(config.uart, buffer, length, portMAX_DELAY);

                        if (read_length > 0) {

                            if (answer_modem) {
                                struct {
                                    const char *p;
                                    const char *q;
                                    int state;
                                } mdm[] = {
                                    { "AT\r", "OK\r", 0 },
                                    { "ATE0V1\r", "OK\r", 0 },
                                    { "ATZ\r", "OK\r", 0 },
                                    { "ATH\r", "OK\r", 0 },
                                    { "ATDT1\r", "OK\r", 2 },
                                };
                                int i;
                                for (i = 0; i < (sizeof(mdm)/sizeof(mdm[0])); i++) {
                                    if (!strcmp(mdm[i].p, buffer)) {
                                        ESP_LOGE(TAG, "recv(%d): %*s", read_length, read_length, buffer); //mdm[i].p);
                                        ESP_LOGE(TAG, "send(%d): %s", strlen(mdm[i].q),  mdm[i].q);
                                        vTaskDelay(100 / portTICK_PERIOD_MS);
                                        uart_write_bytes(config.uart, mdm[i].q, strlen(mdm[i].q));

                                        if (strstr(buffer, "ATDT1")) {
                                            ESP_LOGE(TAG, "send(%d): CONNECT\r", 8);
                                            vTaskDelay(100 / portTICK_PERIOD_MS);
                                            uart_write_bytes(config.uart, "CONNECT\r", 8);
                                            del_ppp_server(ppp_netif);
                                            if (NULL == ppp_netif) {
                                                ppp_netif = new_ppp_server();
                                            } /*else if (current_phase == PPP_PHASE_DEAD) {
                                                ESP_LOGI(TAG, "Connection is dead, restarting ppp interface");
                                                esp_netif_action_start(ppp_netif, NULL, 0, NULL);
                                            }*/
                                            answer_modem = 0;
                                        }

                                        if (strstr(buffer, "ATH") || strstr(buffer, "ATZ")) {

                                            if (NULL != ppp_netif) {
                                                del_ppp_server(ppp_netif);
                                                ppp_netif = NULL;
                                            }
                                            answer_modem = 1;
                                        }
                                    }
                                }

                            } else {

                                if (NULL != ppp_netif) {
                                    esp_err_t res = esp_netif_receive(ppp_netif, buffer, read_length, NULL);
                                    if (res != ESP_OK) {
                                        ESP_LOGE(TAG, "esp_netif_receive error %d", res);
                                        break;
                                    }
                                } else {
                                    ESP_LOGE(TAG, "PPP netif is NULL!");
                                }
                            }
                        }
                    }
                } break;
                case UART_FIFO_OVF: {
                    ESP_LOGW(TAG, "HW FIFO Overflow");
                    uart_flush_input(config.uart);
                    xQueueReset(uart_event_queue);
                } break;
                case UART_BUFFER_FULL: {
                    if (config.uart_config.flow_ctrl == UART_HW_FLOWCTRL_RTS ||
                        config.uart_config.flow_ctrl == UART_HW_FLOWCTRL_CTS_RTS) {
                        ESP_LOGW(TAG, "Ring buffer full, ignoring as flowcontrol is active.");
                    } else {
                        ESP_LOGW(TAG, "Ring Buffer Full, flushing input buffer and event queue.");
                        uart_flush_input(config.uart);
                        xQueueReset(uart_event_queue);
                    }
                } break;
                case UART_BREAK: {
                    ESP_LOGW(TAG, "Rx Break");
                } break;
                case UART_PARITY_ERR: {
                    ESP_LOGE(TAG, "Parity Error");
                } break;
                case UART_FRAME_ERR: {
                    ESP_LOGE(TAG, "Frame Error");
                } break;
                case UART_EVENT_MAX: {
                    if (current_phase != last_phase) {
                        if (last_phase < sizeof(ppp_phase_str) && current_phase < sizeof(ppp_phase_str)) {
                            ESP_LOGE(TAG, "%s(%d) -> %s(%d) mdm:%d", ppp_phase_str[last_phase], last_phase, ppp_phase_str[current_phase], current_phase, answer_modem);
                        }

                        switch (current_phase) {
                            case PPP_PHASE_RUNNING: {
                                wifi_stop();
                                wifi_start();
                                wifi_connect();
                            } break;
                            case PPP_PHASE_DEAD: {
                                wifi_stop();
                                answer_modem = 1;
                                ESP_LOGE(TAG, "MODEM MODE");
                            } break;
                            default: {

                            } break;
                        }
                        last_phase = current_phase;
                    }
                } break;

                default: {
                    ESP_LOGW(TAG, "unknown uart event type: %d", event.type);
                    break;
                }
            }
        }
    }
}

int auth_check_passwd(ppp_pcb *pcb, char *auser, int userlen, char *apasswd, int passwdlen, const char **msg, int *msglen)
{
    memset(&wifi_config, 0, sizeof(wifi_config));
    memcpy(wifi_config.sta.ssid, auser, userlen);
    memcpy(wifi_config.sta.password, apasswd, passwdlen);
    *msg = "Login ok";
    *msglen = sizeof("Login ok")-1;

    ESP_LOGE(TAG, "auth_check_passwd: %s %s", wifi_config.sta.ssid, wifi_config.sta.password);

    return 1;
}

esp_err_t ppp_link_init(const ppp_link_config_t *_config)
{
    config = *_config;

    // Tx buffer needs to be able to contain at least 1 full frame.
    assert(config.buffer.tx_buffer_size >= MAX_PPP_FRAME_SIZE);

    ESP_ERROR_CHECK(uart_param_config(config.uart, &config.uart_config));

    ESP_ERROR_CHECK(uart_set_pin(config.uart, config.io.tx, config.io.rx, config.io.rts, config.io.cts));

    ESP_ERROR_CHECK( uart_driver_install(config.uart, 
                                         config.buffer.rx_buffer_size, 
                                         config.buffer.tx_buffer_size, 
                                         config.buffer.rx_queue_size, 
                                         &uart_event_queue, 0));

    ESP_ERROR_CHECK(uart_set_rx_timeout(config.uart, 1));

    ESP_ERROR_CHECK(uart_set_rx_full_threshold(config.uart, 64));

#if CONFIG_WIN95_MODEM_UART_IRDA_MODE
    ESP_ERROR_CHECK(uart_set_mode(config.uart, UART_MODE_IRDA));
#endif
#if CONFIG_IDF_TARGET_ESP32
    UART1.conf0.tick_ref_always_on = 1;
#endif

    //ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_changed, NULL));

    BaseType_t ret = xTaskCreate(ppp_task_thread, "ppp_task", config.task.stack_size, NULL, config.task.prio, NULL);
    assert(ret == pdTRUE);

    return ESP_OK;
}

static void on_ppp_changed(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    esp_netif_t *netif = *(esp_netif_t **)event_data;

    if (event_id >= NETIF_PP_PHASE_OFFSET && event_id < NETIF_PPP_INTERNAL_ERR_OFFSET) {
        current_phase = (event_id - NETIF_PP_PHASE_OFFSET);
        if (current_phase < sizeof(ppp_phase_str)) {
            ESP_LOGE(TAG, "New PPP phase %s (%d)", ppp_phase_str[current_phase], current_phase);
            if (NULL != uart_event_queue) {

                uart_event_t event = {
                    .type = UART_EVENT_MAX,
                    .size = 0,
                    .timeout_flag = false
                };
                xQueueSend(uart_event_queue, &event, pdMS_TO_TICKS(100));
            }
        }
    }

    switch (event_id) {
        case NETIF_PPP_ERRORNONE: {
            ESP_LOGI(TAG, "No error. from netif: %p", netif);
        } break;
        case NETIF_PPP_ERRORPARAM: {
            ESP_LOGI(TAG, "Invalid parameter. from netif: %p", netif);
        } break;
        case NETIF_PPP_ERROROPEN: {
            ESP_LOGI(TAG, "Unable to open PPP session. from netif: %p", netif);
        } break;
        case NETIF_PPP_ERRORDEVICE: {
            ESP_LOGI(TAG, "Invalid I/O device for PPP. from netif: %p", netif);
        } break;
        case NETIF_PPP_ERRORALLOC: {
            ESP_LOGI(TAG, "Unable to allocate resources. from netif: %p", netif);
        } break;
        case NETIF_PPP_ERRORUSER: {
            ESP_LOGI(TAG, "User interrupted event from netif:%p", netif);
        } break;
        case NETIF_PPP_ERRORCONNECT: {
            ESP_LOGI(TAG, "Connection lost. netif:%p", netif);
        } break;
        case NETIF_PPP_ERRORAUTHFAIL: {
            ESP_LOGI(TAG, "Failed authentication challenge. netif:%p", netif);
        } break;
        case NETIF_PPP_ERRORPROTOCOL: {
            ESP_LOGI(TAG, "Failed to meet protocol. netif:%p", netif);
        } break;
        case NETIF_PPP_ERRORPEERDEAD: {
            ESP_LOGI(TAG, "Connection timeout netif:%p", netif);
        } break;
        case NETIF_PPP_ERRORIDLETIMEOUT: {
            ESP_LOGI(TAG, "Idle Timeout netif:%p", netif);
        } break;
        case NETIF_PPP_ERRORCONNECTTIME: {
            ESP_LOGI(TAG, "Max connect time reached netif:%p", netif);
        } break;
        case NETIF_PPP_ERRORLOOPBACK: {
            ESP_LOGI(TAG, "Loopback detected netif:%p", netif);
        } break;
        case NETIF_PPP_PHASE_DEAD: {
            ESP_LOGI(TAG, "Phase Dead");
        } break;
        case NETIF_PPP_PHASE_INITIALIZE: {
            ESP_LOGI(TAG, "Phase Start");
        } break;
        case NETIF_PPP_PHASE_ESTABLISH: {
            ESP_LOGI(TAG, "Phase Establish");
        } break;
        case NETIF_PPP_PHASE_AUTHENTICATE: {
            ESP_LOGI(TAG, "Phase Authenticate");
        } break;
        case NETIF_PPP_PHASE_NETWORK: {
            ESP_LOGI(TAG, "Phase Network");
        } break;
        case NETIF_PPP_PHASE_RUNNING: {
            ESP_LOGI(TAG, "Phase Running");
        } break;
        case NETIF_PPP_PHASE_TERMINATE: {
            ESP_LOGI(TAG, "Phase Terminate");
        } break;
        case NETIF_PPP_PHASE_DISCONNECT: {
            ESP_LOGI(TAG, "Phase Disconnect");
        } break;
        default: {
            ESP_LOGI(TAG, "Unknown PPP event %d", event_id);
            break;
         }
    }
}

static void on_wifi_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id) {

        case WIFI_EVENT_STA_DISCONNECTED: {
			if (NULL != ppp_netif) {
				ESP_LOGE("wifi", "Wifi disconnected, stopping PPP...");
				esp_netif_action_stop(ppp_netif, event_base, event_id, event_data);
			}
        } break;

        default: {
			ESP_LOGI(TAG, "Unknown WIFI event %d", event_id);
        } break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{

    switch (event_id) {
        case IP_EVENT_PPP_GOT_IP: {
            esp_netif_dns_info_t dns_info;

            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            esp_netif_t *netif = event->esp_netif;
            esp_netif_action_connected(netif, event_base, event_id, event_data);

            ESP_LOGI(TAG, "Modem Connect to PPP endpoint %p -- %p", arg, netif);
            ESP_LOGI(TAG, "--------------");
            ESP_LOGI(TAG, "IP          : " IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "Netmask     : " IPSTR, IP2STR(&event->ip_info.netmask));
            ESP_LOGI(TAG, "Gateway     : " IPSTR, IP2STR(&event->ip_info.gw));
            esp_netif_get_dns_info(netif, 0, &dns_info);
            ESP_LOGI(TAG, "Name Server1: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
            esp_netif_get_dns_info(netif, 1, &dns_info);
            ESP_LOGI(TAG, "Name Server2: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
            ESP_LOGI(TAG, "--------------");

            //struct netif *ppp_netif_impl = esp_netif_get_netif_impl(netif);
            //assert(ppp_netif_impl);
            //ip_napt_enable_netif(ppp_netif_impl, 1);
            ip_napt_enable(event->ip_info.ip.addr, 1);
            ESP_LOGI(TAG, "NAPT enabled");
        } break;

        case IP_EVENT_PPP_LOST_IP: {
            ESP_LOGI(TAG, "Modem Disconnect from PPP Server %p", arg);
            esp_netif_action_disconnected(arg, event_base, event_id, event_data);
        } break;

        case IP_EVENT_STA_GOT_IP: {
            esp_netif_dns_info_t dns_info;

            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            esp_netif_t *netif = event->esp_netif;

            ESP_LOGI(TAG, "WIFI got connected");
            ESP_LOGI(TAG, "--------------");
            ESP_LOGI(TAG, "IP          : " IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "Netmask     : " IPSTR, IP2STR(&event->ip_info.netmask));
            ESP_LOGI(TAG, "Gateway     : " IPSTR, IP2STR(&event->ip_info.gw));
            esp_netif_get_dns_info(netif, 0, &dns_info);
            ESP_LOGI(TAG, "Name Server1: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
            esp_netif_get_dns_info(netif, 1, &dns_info);
            ESP_LOGI(TAG, "Name Server2: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
            ESP_LOGI(TAG, "--------------");

            struct netif *sta_netif_impl = esp_netif_get_netif_impl(netif);
            assert(sta_netif_impl);
            //ip_napt_enable_netif(sta_netif_impl, 1);
            //ip_napt_enable(event->ip_info.ip.addr, 1);
            //ESP_LOGI(TAG, "NAPT enabled");

            netif_set_default(sta_netif_impl);

            /* proxy arp */
#if 0
            ip4_addr_t ppp_remote_addr;
            ppp_remote_addr.addr = esp_netif_htonl(esp_netif_ip4_makeu32(10, 10, 0, 2));
            netif_set_proxyarp(sta_netif_impl, &ppp_remote_addr);
            ESP_LOGI(TAG, "proxy ARP enabled");
#endif

            //struct eth_addr esp_mac;
            //esp_read_mac(esp_mac.addr, ESP_MAC_WIFI_STA);
            //etharp_add_static_entry(&ppp_remote_addr, &esp_mac);

            sta_connected = true;

        } break;


        default: {
            ESP_LOGE(TAG, "UNHANDLED IP event! %d", event_id);
        } break;
    }
}

#ifdef CONFIG_PPP_SERVER_SUPPORT
static int cmd_ppp_server(int argc, char **argv)
{
    ppp_link_config_t ppp_link_config = DEFAULT_LINK_CONFIG;
    ppp_link_config.type = PPP_LINK_SERVER;
    ppp_link_config.ppp_server.localaddr.addr = esp_netif_htonl(esp_netif_ip4_makeu32(10, 10, 0, 1));
    ppp_link_config.ppp_server.remoteaddr.addr = esp_netif_htonl(esp_netif_ip4_makeu32(10, 10, 0, 2));

    ppp_link_config.ppp_server.dnsaddr1.addr = esp_netif_htonl(esp_netif_ip4_makeu32(1,1,1,1));
    ppp_link_config.ppp_server.dnsaddr2.addr = esp_netif_htonl(esp_netif_ip4_makeu32(0, 0, 0, 0));

    ppp_link_config.ppp_server.login = "esp";
    ppp_link_config.ppp_server.password = "esp";
    ppp_link_config.ppp_server.auth_req = !0;

    ESP_LOGI(TAG, "Will configure as PPP SERVER");
    ppp_link_init(&ppp_link_config);

    return 0;
}
#endif

static int cmd_ppp_client(int argc, char **argv)
{
    ppp_link_config_t ppp_link_config = DEFAULT_LINK_CONFIG;
    ppp_link_config.type = PPP_LINK_CLIENT;
    ESP_LOGI(TAG, "Will configure as PPP CLIENT");
    ppp_link_init(&ppp_link_config);
    return 0;
}

static int cmd_cli_server(int argc, char **argv)
{
    cli_server_config_t cli_server = DEFAULT_CLI_SERVER_CONFIG;

    cli_server_init(&cli_server);
    return 0;
}

static void initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "ppp_server>";
#if CONFIG_IDF_TARGET_ESP32
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    // init console REPL environment
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));
#elif CONFIG_IDF_TARGET_ESP32C3
    esp_console_dev_usb_serial_jtag_config_t uart_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    // init console REPL environment
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&uart_config, &repl_config, &repl));
#endif
    esp_log_level_set("esp_netif_lwip", ESP_LOG_VERBOSE);
    esp_log_level_set("ppp_link", ESP_LOG_VERBOSE);
    esp_log_level_set("ppp_server_main", ESP_LOG_VERBOSE);
    esp_log_level_set("cli_server", ESP_LOG_VERBOSE);
    esp_log_level_set("esp-netif_lwip-ppp", ESP_LOG_VERBOSE);
    esp_log_level_set("*", ESP_LOG_INFO);


    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    initialize_nvs();
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_changed, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_event, NULL));

    /* Register commands */
    register_system_common();
    register_wifi();
    register_iperf();
    register_ping();


#ifdef CONFIG_PPP_SERVER_SUPPORT
    const esp_console_cmd_t ppp_server = {
        .command = "ppp_server",
        .help = "Start ppp server",
        .hint = NULL,
        .func = &cmd_ppp_server,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ppp_server));
#endif
    const esp_console_cmd_t ppp_client = {
        .command = "ppp_client",
        .help = "Start ppp client",
        .hint = NULL,
        .func = &cmd_ppp_client,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&ppp_client));

    const esp_console_cmd_t cli_server_cmd = {
        .command = "cli_server",
        .help = "Start cli server",
        .hint = NULL,
        .func = &cmd_cli_server,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cli_server_cmd));


    cli_client_config_t client_config = DEFAULT_CLI_CLIENT_CONFIG;
    cli_client_init(&client_config);

    /* Sleep forever */
//   ESP_LOGI(TAG, " ============================================================");
//   ESP_LOGI(TAG, " |       Steps to Test Bandwidth                             |");
//   ESP_LOGI(TAG, " |                                                           |");
//   ESP_LOGI(TAG, " |  1. Run ppp_server or ppp_client                          |");
//   ESP_LOGI(TAG, " |  2. Wait ESP32 to get IP from DHCP                        |");
//   ESP_LOGI(TAG, " |  3. ping -c 10 10.10.0.2                                  |");
//   ESP_LOGI(TAG, " |  4. Server UDP: 'iperf -u -s 10.10.0.1 -i 3'              |");
//   ESP_LOGI(TAG, " |  5. Client UDP: 'iperf -u -c 10.10.0.1 -t 60 -i 3'        |");
//   ESP_LOGI(TAG, " |  6. Server TCP: 'iperf -s 10.10.0.1 -i 3'                 |");
//   ESP_LOGI(TAG, " |  7. Client TCP: 'iperf -c 10.10.0.1 -t 60 -i 3'           |");
//   ESP_LOGI(TAG, " |                                                           |");
//   ESP_LOGI(TAG, " =============================================================");
//

    int cmd_ret = 0;
    ESP_ERROR_CHECK(esp_console_run("ppp_server", &cmd_ret));

    // start console REPL
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
/*
vim:ts=4:sw=4:cc=80:et
*/
