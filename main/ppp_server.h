/* $Id$ */
#ifndef __PPP_SERVER_H__
#define __PPP_SERVER_H__

#include "esp_netif.h"
#include "driver/uart.h"
#include "hal/gpio_types.h"

struct ppp_link_config_s {
    enum {
        PPP_LINK_CLIENT,
#ifdef CONFIG_PPP_SERVER_SUPPORT
        PPP_LINK_SERVER,
#endif
    } type;
    uart_port_t uart;
    uart_config_t uart_config;
    struct {
        gpio_num_t tx;
        gpio_num_t rx;
        gpio_num_t rts;
        gpio_num_t cts;
    } io;
    struct {
        int rx_buffer_size;
        int tx_buffer_size;
        int rx_queue_size;
    } buffer;
    struct {
        int stack_size;
        int prio;
    } task;
#ifdef CONFIG_PPP_SERVER_SUPPORT
    struct {
        esp_ip4_addr_t localaddr;
        esp_ip4_addr_t remoteaddr;
        esp_ip4_addr_t dnsaddr1;
        esp_ip4_addr_t dnsaddr2;
        const char *login;
        const char *password;
        int auth_req;
    } ppp_server;
#endif
};

typedef struct ppp_link_config_s ppp_link_config_t;


#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2
/*#define UART_CLK UART_SCLK_REF_TICK*/
#define UART_CLK UART_SCLK_APB
#else
//#define UART_CLK UART_SCLK_XTAL
#define UART_CLK UART_SCLK_APB
#endif

#define DEFAULT_LINK_CONFIG                                                   \
    {.type = PPP_LINK_SERVER,                                                 \
     .uart = UART_NUM_1,                                                      \
     .uart_config =                                                           \
         {                                                                    \
             .baud_rate = CONFIG_WIN95_MODEM_UART_BAUDRATE,                   \
             .data_bits = UART_DATA_8_BITS,                                   \
             .parity = UART_PARITY_DISABLE,                                   \
             .stop_bits = UART_STOP_BITS_1,                                   \
             .source_clk = UART_CLK,                                          \
             .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,                           \
             .rx_flow_ctrl_thresh = UART_FIFO_LEN - 8,                        \
         },                                                                   \
     .io = {.tx = CONFIG_WIN95_MODEM_UART_TX_PIN,                             \
            .rx = CONFIG_WIN95_MODEM_UART_RX_PIN,                             \
            .rts = CONFIG_WIN95_MODEM_UART_RTS_PIN,                           \
            .cts = CONFIG_WIN95_MODEM_UART_CTS_PIN                            \
     },                                                                       \
     .buffer = {.rx_buffer_size = CONFIG_WIN95_MODEM_UART_RX_BUFFER_SIZE,     \
                .tx_buffer_size = CONFIG_WIN95_MODEM_UART_TX_BUFFER_SIZE,     \
                .rx_queue_size = CONFIG_WIN95_MODEM_UART_EVENT_QUEUE_SIZE     \
     },                                                                       \
     .task = {                                                                \
         .stack_size = CONFIG_WIN95_MODEM_UART_EVENT_TASK_STACK_SIZE,         \
         .prio = CONFIG_WIN95_MODEM_UART_EVENT_TASK_PRIORITY,                 \
     }};

#define MAX_PPP_FRAME_SIZE (PPP_MAXMRU + 10) // 10 bytes of ppp framing around max 1500 bytes information


esp_err_t ppp_link_init(const ppp_link_config_t *ppp_link_config);

#endif /* __PPP_SERVER_H__ */
/*
vim:ts=4:sw=4:cc=80:et
*/
