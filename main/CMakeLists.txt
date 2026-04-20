idf_component_register(SRCS 
                    "ppp_server.c" 
                    INCLUDE_DIRS "."
					PRIV_REQUIRES cli_client cli_server cmd_iperf cmd_ping cmd_system cmd_wifi iperf
					REQUIRES driver esp_netif esp_wifi console nvs_flash fatfs mqtt)
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
