#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_bt.h>
#include <esp_mac.h>
#include <mbedtls/aes.h>
#include "gamepad_protocol.h"

// Only wake signalling is handled. The existing Linux HID pairing is retained.
class GamepadWake {
public:
    struct Peer { uint8_t address[6]={};uint8_t irk[16]={};bool hasIrk=false;char name[40]={},family[8]={}; };
private:
    struct Packet {uint16_t length;uint8_t data[260];};
    struct Command {uint16_t opcode;uint8_t size;uint8_t data[8];};
    Peer peers[6];unsigned count=0;
    uint8_t hostMac[6]={},stationMac[6]={};
    bool configured=false,identityOK=false,active=false,busy=false,standby=false,armed=false,wake=false;
    bool rejectedPending=false,bleFilterFallback=false;
    uint8_t rejectAddress[6]={};
    uint32_t lastCommandAt=0,standbyAt=0,lastSignalAt=0,minimumArmAt=0,lastWakeAt=0,lastStartTry=0;
    unsigned initStep=0;
    uint16_t outstanding=0;
    uint8_t lastStatus=0;
    uint32_t requests=0,unknownRequests=0;
    QueueHandle_t packets=nullptr;
    String error,lastPeer;
    static GamepadWake*& instance(){static GamepadWake* value=nullptr;return value;}
    static void available(){}
    static int receive(uint8_t* data,uint16_t length){
        GamepadWake* self=instance();
        if(self&&self->packets&&length<=260&&length>=3&&data[0]==4){
            Packet packet;packet.length=length;memcpy(packet.data,data,length);xQueueSend(self->packets,&packet,0);
        }
        return 0;
    }
    static String macText(const uint8_t address[6]){
        char text[18];snprintf(text,sizeof(text),"%02X:%02X:%02X:%02X:%02X:%02X",address[0],address[1],address[2],address[3],address[4],address[5]);return text;
    }
    bool send(uint16_t opcode,const uint8_t* params=nullptr,uint8_t length=0){
        if(!active||busy||length>40||!esp_vhci_host_check_send_available())return false;
        uint8_t data[44]={1,static_cast<uint8_t>(opcode),static_cast<uint8_t>(opcode>>8),length};
        if(length)memcpy(data+4,params,length);
        busy=true;outstanding=opcode;lastCommandAt=millis();esp_vhci_host_send_packet(data,4+length);return true;
    }
    static bool resolvePrivate(const Peer& peer,const uint8_t address[6]){
        if(!peer.hasIrk||(address[0]&0xc0)!=0x40)return false;
        uint8_t input[16]={},output[16]={},key[16];memcpy(input+13,address,3);
        for(int order=0;order<2;order++){
            for(int i=0;i<16;i++)key[i]=peer.irk[order?15-i:i];
            mbedtls_aes_context aes;mbedtls_aes_init(&aes);mbedtls_aes_setkey_enc(&aes,key,128);
            mbedtls_aes_crypt_ecb(&aes,MBEDTLS_AES_ENCRYPT,input,output);mbedtls_aes_free(&aes);
            if(!memcmp(output+13,address+3,3))return true;
        }
        return false;
    }
    int findPeer(const uint8_t address[6],bool ble){
        for(unsigned i=0;i<count;i++)if(!memcmp(peers[i].address,address,6)||(ble&&resolvePrivate(peers[i],address)))return i;
        return -1;
    }
    void signal(const uint8_t address[6],bool ble,int8_t rssi=0){
        int index=findPeer(address,ble);
        if(index<0){if(!ble)unknownRequests++;return;}
        if(ble&&(rssi==127||rssi< -88))return;
        requests++;
        bool canWake=armed&&standby&&identityOK;
        lastSignalAt=millis();
        if(canWake){wake=true;armed=false;lastPeer=peers[index].name;lastWakeAt=millis();}
    }
    void handle(const Packet& packet){
        const uint8_t* p=packet.data;size_t n=packet.length;
        if(n<3||static_cast<size_t>(p[2])+3>n)return;
        if((p[1]==0x0e&&p[2]>=4)||(p[1]==0x0f&&p[2]>=4)){
            uint16_t opcode=p[1]==0x0e?(p[4]|p[5]<<8):(p[5]|p[6]<<8);
            uint8_t status=p[1]==0x0e?p[6]:p[3];
            if(opcode==outstanding){
                busy=false;lastStatus=status;
                if(status&&opcode==0x200b&&!bleFilterFallback){bleFilterFallback=true;return;}
                if(status){error="HCI "+String(opcode,HEX)+" / "+String(status,HEX);return;}
                if(opcode==0x1009&&p[1]==0x0e&&p[2]>=10){
                    uint8_t actual[6];for(int i=0;i<6;i++)actual[i]=p[12-i];
                    identityOK=!memcmp(actual,hostMac,6);if(!identityOK)error="Bluetooth identity mismatch";
                }
                if(opcode!=0x040a)initStep++;
            }
        }
        uint8_t address[6];
        if(GamepadProtocol::connectionRequest(p,n,address)){
            memcpy(rejectAddress,address,6);rejectedPending=true;signal(address,false);
        }
        GamepadProtocol::advertisements(p,n,[&](const uint8_t* address,uint8_t,int8_t rssi){signal(address,true,rssi);});
    }
    bool initializeCommand(){
        static const Command commands[]={
            {0x0c03,0,{}},{0x1009,0,{}},
            {0x0c01,8,{0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x3f}},
            {0x0c1c,4,{0x00,0x01,0x12,0x00}},
            {0x0c1a,1,{0x02}},
            {0x2001,8,{0x02,0x04,0,0,0,0,0,0}},
            {0x200b,7,{0,0x80,0,0x30,0,0,2}},
            {0x200c,2,{1,0}}
        };
        if(initStep>=sizeof(commands)/sizeof(commands[0]))return true;
        Command command=commands[initStep];if(command.opcode==0x200b&&bleFilterFallback)command.data[6]=0;
        send(command.opcode,command.data,command.size);return false;
    }
public:
    bool configure(JsonObject doc){
        uint8_t host[6];if(!GamepadProtocol::address(doc["host_mac"]|"",host))return false;
        JsonArray list=doc["controllers"];if(list.isNull()||list.size()>6)return false;
        Peer next[6];unsigned used=0;
        for(JsonObject item:list){
            const char* family=item["family"]|"";
            if(strcmp(family,"switch")&&strcmp(family,"ps5")&&strcmp(family,"xbox"))return false;
            Peer& peer=next[used];if(!GamepadProtocol::address(item["address"]|"",peer.address))return false;
            snprintf(peer.name,sizeof(peer.name),"%s",item["name"]|family);snprintf(peer.family,sizeof(peer.family),"%s",family);
            const char* key=item["irk"]|"";
            if(strlen(key)==32){
                peer.hasIrk=true;
                for(int i=0;i<32;i++)if(!isxdigit(static_cast<unsigned char>(key[i])))return false;
                for(int i=0;i<16;i++){char pair[3]={key[i*2],key[i*2+1],0};peer.irk[i]=strtoul(pair,nullptr,16);}
            }else if(strlen(key))return false;
            used++;
        }
        memcpy(hostMac,host,6);memcpy(peers,next,sizeof(peers));count=used;configured=true;return true;
    }
    bool prepareIdentity(){
        esp_efuse_mac_get_default(stationMac);
        if(!configured||!count)return true;
        uint8_t base[6];memcpy(base,hostMac,6);
        unsigned subtract=CONFIG_ESP32_UNIVERSAL_MAC_ADDRESSES==4?2:1;
        // Supported ESP32 derivation increments the final octet, without wrapping.
        if(base[5]<subtract){error="Unsupported Bluetooth identity";return false;}
        base[5]-=subtract;
        if(esp_base_mac_addr_set(base)!=ESP_OK){error="Cannot set standby identity";return false;}
        uint8_t actual[6];esp_read_mac(actual,ESP_MAC_BT);identityOK=!memcmp(actual,hostMac,6);
        return identityOK;
    }
    const uint8_t* originalStationMac()const{return stationMac;}
    String hostIdentity()const{return macText(hostMac);}
    bool needsIdentityRestart()const{return configured&&count&&!identityOK;}
    bool reserveController(){
        if(!configured||!count)return true;
        if(esp_bt_controller_get_status()==ESP_BT_CONTROLLER_STATUS_IDLE){
            esp_bt_controller_config_t cfg=BT_CONTROLLER_INIT_CONFIG_DEFAULT();
            esp_err_t result=esp_bt_controller_init(&cfg);
            if(result!=ESP_OK){error="Bluetooth init: "+String(result);return false;}
            error="";
        }
        return true;
    }
    bool stop(){
        standby=false;armed=false;wake=false;
        if(active&&esp_bt_controller_get_status()==ESP_BT_CONTROLLER_STATUS_ENABLED){
            esp_err_t result=esp_bt_controller_disable();
            if(result!=ESP_OK){error="Cannot stop standby radio: "+String(result);return false;}
        }
        active=false;
        busy=false;initStep=0;outstanding=0;rejectedPending=false;
        if(packets)xQueueReset(packets);
        return true;
    }
    void suppressFor(uint32_t delay){minimumArmAt=millis()+delay;armed=false;lastSignalAt=millis();}
    void poll(bool mayListen){
        if(!mayListen||!configured||!count){if(active)stop();return;}
        if(!active){
            if(millis()-lastStartTry<1500)return;lastStartTry=millis();
            if(!identityOK){error="Standby identity needs restart";return;}
            if(!packets)packets=xQueueCreate(12,sizeof(Packet));
            if(!packets){error="Bluetooth queue unavailable";return;}
            instance()=this;
            if(!reserveController())return;
            if(esp_bt_controller_enable(ESP_BT_MODE_BTDM)!=ESP_OK){error="Bluetooth enable failed";return;}
            static const esp_vhci_host_callback_t callbacks={available,receive};
            esp_vhci_host_register_callback(&callbacks);
            active=true;standby=true;initStep=0;busy=false;error="";standbyAt=lastSignalAt=millis();
        }
        Packet packet;for(int i=0;i<12&&xQueueReceive(packets,&packet,0)==pdTRUE;i++)handle(packet);
        if(busy&&millis()-lastCommandAt>2500){error="Bluetooth command timeout";stop();return;}
        // Retry failed initialization instead of staying permanently deaf until reset.
        if(error.length()){stop();return;}
        if(rejectedPending){
            uint8_t params[7];for(int i=0;i<6;i++)params[i]=rejectAddress[5-i];params[6]=0x0d;
            if(send(0x040a,params,7))rejectedPending=false;
        }else if(initializeCommand()&&identityOK){
            armed=!wake&&millis()-standbyAt>=8000&&millis()-lastSignalAt>=8000&&(!minimumArmAt||static_cast<int32_t>(millis()-minimumArmAt)>=0);
        }
    }
    bool takeWake(){bool result=wake&&!busy&&!rejectedPending;if(result)wake=false;return result;}
    void status(JsonObject doc){
        doc["enabled"]=configured&&count;doc["listening"]=active;doc["armed"]=armed;
        doc["host_mac"]=hostIdentity();doc["error"]=error;
        doc["controller_status"]=static_cast<int>(esp_bt_controller_get_status());
        doc["identity_ok"]=identityOK;doc["init_step"]=initStep;doc["last_hci_status"]=lastStatus;doc["last_controller"]=lastPeer;
        doc["last_wake_ms"]=lastWakeAt;doc["signals_seen"]=requests;doc["unknown_requests"]=unknownRequests;
        JsonArray list=doc.createNestedArray("controllers");
        for(unsigned i=0;i<count;i++){JsonObject item=list.createNestedObject();item["address"]=macText(peers[i].address);item["name"]=peers[i].name;item["family"]=peers[i].family;item["private_address_supported"]=peers[i].hasIrk;}
    }
};
