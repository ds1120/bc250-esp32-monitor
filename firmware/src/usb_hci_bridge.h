#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_bt.h>
#include <freertos/semphr.h>
#include "hci_h4.h"
#include "usb_monitor_protocol.h"
#include "usb_info_protocol.h"

// Controller only. Linux BlueZ supplies the Bluetooth host stack over USB-UART.
// GamepadWake and this bridge must never own the radio at the same time.
class UsbHciBridge {
    struct Packet{uint16_t length;uint8_t data[HciH4Parser::CAPACITY];};
    QueueHandle_t queue=nullptr,monitorQueue=nullptr,infoQueue=nullptr;
    Packet output{};size_t offset=0;
    HciH4Parser input;
    volatile bool active=false,requested=false,restartNotice=false;
    volatile uint32_t monitorInterval=5000;
    SemaphoreHandle_t mutex=nullptr;
    TaskHandle_t task=nullptr;
    uint32_t lastByteAt=0,lastStartTry=0;
    volatile uint32_t dropped=0;
    uint32_t sent=0,received=0,invalid=0;
    String error;
    static UsbHciBridge*& instance(){static UsbHciBridge* p=nullptr;return p;}
    static void available(){}
    static int receive(uint8_t* bytes,uint16_t length){
        auto self=instance();
        if(!self||!self->queue)return 0;
        if(!length||length>HciH4Parser::CAPACITY){self->dropped++;return 0;}
        Packet packet;packet.length=length;memcpy(packet.data,bytes,length);
        if(xQueueSend(self->queue,&packet,0)!=pdTRUE)self->dropped++;
        return 0;
    }
    static void worker(void* arg){
        auto self=static_cast<UsbHciBridge*>(arg);
        for(;;){
            xSemaphoreTake(self->mutex,portMAX_DELAY);
            self->service(self->requested);
            xSemaphoreGive(self->mutex);
            vTaskDelay(1);
        }
    }
public:
    bool begin(){
        if(task)return true;
        if(esp_bt_controller_get_status()==ESP_BT_CONTROLLER_STATUS_IDLE){
            esp_bt_controller_config_t cfg=BT_CONTROLLER_INIT_CONFIG_DEFAULT();
            esp_err_t result=esp_bt_controller_init(&cfg);
            if(result!=ESP_OK){error="controller_init: "+String(result);return false;}
        }
        if(!queue)queue=xQueueCreate(6,sizeof(Packet));
        if(!monitorQueue)monitorQueue=xQueueCreate(1,sizeof(UsbMonitorProtocol::Sample));
        if(!infoQueue)infoQueue=xQueueCreate(1,sizeof(UsbInfoProtocol::Info));
        if(!mutex)mutex=xSemaphoreCreateMutex();
        if(!queue||!monitorQueue||!infoQueue||!mutex){error="bridge_allocation_failed";return false;}
        if(xTaskCreatePinnedToCore(worker,"usb-hci",4096,this,2,&task,1)!=pdPASS){error="bridge_task_failed";return false;}
        return true;
    }
    void poll(bool shouldRun){requested=shouldRun;}
    static constexpr uint32_t BAUD=921600;
    bool running()const{return active;}
    void notifyHostRestart(){restartNotice=true;}
    void setMonitorInterval(uint32_t interval){monitorInterval=interval;}
    bool takeMonitor(UsbMonitorProtocol::Sample& sample){return monitorQueue&&xQueueReceive(monitorQueue,&sample,0)==pdTRUE;}
    bool takeInfo(UsbInfoProtocol::Info& info){return infoQueue&&xQueueReceive(infoQueue,&info,0)==pdTRUE;}
    bool stop(){
        if(active&&esp_bt_controller_get_status()==ESP_BT_CONTROLLER_STATUS_ENABLED){
            esp_err_t result=esp_bt_controller_disable();
            if(result!=ESP_OK){error="disable: "+String(result);return false;}
        }
        active=false;input.reset();output.length=0;offset=0;
        if(queue)xQueueReset(queue);
        return true;
    }
    void service(bool shouldRun){
        if(!shouldRun){
            stop();
            // Linux may leave final HCI commands in USB RX at shutdown.
            for(unsigned i=0;i<1024&&Serial.available();i++)Serial.read();
            return;
        }
        if(!active){
            if(millis()-lastStartTry<1000)return;
            lastStartTry=millis();
            if(!queue)queue=xQueueCreate(6,sizeof(Packet));
            if(!queue){error="queue_allocation_failed";return;}
            if(esp_bt_controller_get_status()!=ESP_BT_CONTROLLER_STATUS_INITED){error="radio_not_released";return;}
            instance()=this;
            static const esp_vhci_host_callback_t callbacks={available,receive};
            esp_err_t result=esp_vhci_host_register_callback(&callbacks);
            if(result!=ESP_OK){error="callback: "+String(result);return;}
            result=esp_bt_controller_enable(ESP_BT_MODE_BTDM);
            if(result!=ESP_OK){error="enable: "+String(result);return;}
            active=true;error="";input.reset();output.length=0;offset=0;
            if(restartNotice){
                // Standard HCI Hardware Error tells Linux to reinitialize after
                // the external controller restarted while its USB-UART stayed up.
                uint8_t event[]={4,0x10,1,1};receive(event,sizeof(event));restartNotice=false;
            }
        }
        if(input.partial()&&millis()-lastByteAt>500){input.reset();invalid++;}
        // Bounded work per loop keeps touch, Wi-Fi and authenticated shutdown responsive.
        for(unsigned budget=0;budget<1024;budget++){
            if(input.ready()){
                const uint8_t* bytes=input.data();
                bool monitorCommand=bytes[0]==1&&bytes[1]==(UsbMonitorProtocol::OPCODE&255)&&bytes[2]==(UsbMonitorProtocol::OPCODE>>8)&&input.size()>=4;
                if(monitorCommand){
                    UsbMonitorProtocol::Sample sample;
                    bool valid=UsbMonitorProtocol::decode(bytes+4,input.size()-4,sample);
                    if(valid&&monitorQueue)xQueueOverwrite(monitorQueue,&sample);
                    uint8_t reply[]={4,0x0e,6,1,static_cast<uint8_t>(UsbMonitorProtocol::OPCODE),static_cast<uint8_t>(UsbMonitorProtocol::OPCODE>>8),static_cast<uint8_t>(valid?0:0x12),static_cast<uint8_t>(monitorInterval),static_cast<uint8_t>(monitorInterval>>8)};
                    receive(reply,sizeof(reply));
                }else if(bytes[0]==1&&bytes[1]==(UsbInfoProtocol::OPCODE&255)&&bytes[2]==(UsbInfoProtocol::OPCODE>>8)){
                    UsbInfoProtocol::Info info;bool valid=UsbInfoProtocol::decode(bytes+4,input.size()-4,info);
                    if(valid&&infoQueue)xQueueOverwrite(infoQueue,&info);
                    uint8_t reply[]={4,0x0e,4,1,0xf1,0xfc,static_cast<uint8_t>(valid?0:0x12)};receive(reply,sizeof(reply));
                }else {if(!esp_vhci_host_check_send_available())break;esp_vhci_host_send_packet(const_cast<uint8_t*>(bytes),input.size());}
                sent++;input.reset();
            }
            if(!Serial.available())break;
            if(!input.push(static_cast<uint8_t>(Serial.read())))invalid++;
            lastByteAt=millis();
        }
        for(unsigned budget=0;budget<6;budget++){
            if(offset==output.length){
                if(xQueueReceive(queue,&output,0)!=pdTRUE)break;
                offset=0;received++;
            }
            int room=Serial.availableForWrite();if(room<=0)break;
            size_t remaining=output.length-offset;
            size_t chunk=remaining<static_cast<size_t>(room)?remaining:static_cast<size_t>(room);
            offset+=Serial.write(output.data+offset,chunk);
            if(offset<output.length)break;
        }
    }
    void status(JsonObject doc,bool enabled){
        bool locked=mutex&&xSemaphoreTake(mutex,pdMS_TO_TICKS(10))==pdTRUE;
        doc["supported"]=true;doc["enabled"]=enabled;doc["active"]=active;
        doc["baud"]=BAUD;doc["transport"]="h4_usb_uart";
        doc["host_packets"]=sent;doc["controller_packets"]=received;
        doc["dropped_packets"]=static_cast<uint32_t>(dropped);doc["invalid_bytes"]=invalid;
        if(!mutex||locked)doc["error"]=error;
        else doc["error"]="status_busy";
        if(locked)xSemaphoreGive(mutex);
    }
};
