#pragma once
#include <Arduino.h>
using esp_err_t=int;
constexpr int ESP_OK=0,ESP_BT_MODE_BTDM=3,ESP_BT_CONTROLLER_STATUS_IDLE=0,ESP_BT_CONTROLLER_STATUS_INITED=1,ESP_BT_CONTROLLER_STATUS_ENABLED=2;
struct esp_bt_controller_config_t{};
#define BT_CONTROLLER_INIT_CONFIG_DEFAULT() esp_bt_controller_config_t{}
struct esp_vhci_host_callback_t{void(*notify_host_send_available)();int(*notify_host_recv)(uint8_t*,uint16_t);};
inline int mockRadioState=0,mockDisableError=0;inline bool mockCanSend=true;
inline const esp_vhci_host_callback_t* mockCallback=nullptr;
inline std::vector<std::vector<uint8_t>> mockCommands;
inline int esp_bt_controller_get_status(){return mockRadioState;}
inline int esp_bt_controller_init(esp_bt_controller_config_t*){mockRadioState=1;return 0;}
inline int esp_bt_controller_enable(int){mockRadioState=2;return 0;}
inline int esp_bt_controller_disable(){if(mockDisableError)return mockDisableError;mockRadioState=1;return 0;}
inline int esp_vhci_host_register_callback(const esp_vhci_host_callback_t* cb){mockCallback=cb;return 0;}
inline bool esp_vhci_host_check_send_available(){return mockCanSend;}
inline void esp_vhci_host_send_packet(uint8_t* bytes,size_t length){mockCommands.emplace_back(bytes,bytes+length);}
