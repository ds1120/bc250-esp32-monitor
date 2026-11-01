#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <math.h>
#include <driver/gpio.h>
#include <soc/gpio_reg.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <esp_ota_ops.h>
#include <Update.h>
#include <SD.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_system.h>
#include <hal/gpio_ll.h>
#include "relay_restart.h"
#include "relay_hold_guard.h"
#include "gamepad_wake.h"
#include "usb_hci_bridge.h"
#include "settings_merge.h"

// Keep controller memory: the raw VHCI host does not pull in Arduino btStart().
extern "C" bool btInUse(){return true;}

// GPIO22 drives relay IN (H mode). Boot OFF; explicit ON holds until OFF/reset.
// Only an authenticated, current-boot final poweroff signal schedules automatic OFF.
constexpr const char* FIRMWARE_VERSION="bc250-monitor-5.7";
TFT_eSPI lcd;
TFT_eSprite lineSprite(&lcd);
TFT_eSprite metricSprite(&lcd),animationSprite(&lcd);
SPIClass sdSPI(VSPI);
bool sdReady=false;
String sdError;
void savePadSD();
XPT2046_Touchscreen touch(33,36);
WebServer web(80);
WiFiUDP shutdownUDP;
Preferences config;
GamepadWake gamepadWake;
UsbHciBridge usbHci;
RTC_NOINIT_ATTR RelayRestartTicket restartTicket;
bool relayResumedAfterRestart=false;
bool usbHciEnabled=false;
void consolePrintln(const String& value=""){if(!usbHciEnabled)Serial.println(value);}
template<typename... Args> void consolePrintf(const char* format,Args... args){if(!usbHciEnabled)Serial.printf(format,args...);}
String defaultGamepadConfig(){
    uint8_t mac[6];esp_read_mac(mac,ESP_MAC_BT);char value[96];
    snprintf(value,sizeof(value),"{\"host_mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"controllers\":[]}",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);return String(value);
}
String gamepadConfig;
bool wakeConfigNeedsRestart=false;
bool gamepadWakeDisabled=false;
bool gamepadManual=false;
bool padPowerPending=false;
uint32_t padPowerStarted=0,padPowerLastTry=0,padPowerAttempts=0,padPowerAcceptedAt=0;
String padPowerResult="none",padPowerError,relayOnError;
void padPowerStatus(JsonObject doc){
    doc["power_pending"]=padPowerPending;doc["power_attempts"]=padPowerAttempts;
    doc["power_result"]=padPowerResult;doc["power_accepted_ms"]=padPowerAcceptedAt;
    doc["power_error"]=padPowerError;
}
QueueHandle_t wakeConfigs;
struct WakeConfigMessage {char json[2048];};
struct Calibration { uint32_t magic; float ax,bx,cx,ay,by,cy; } cal{};
bool calibrated=false, downBefore=false, configured=false;
String ssid,password,token,pin;
String host="",learnedPcName="",learnedBluetooth="";
uint16_t agentPort=9091;
bool runtimeInfoNeedsSave=false;
String serialLine, confirmation;
uint32_t confirmationAt=0, lastTouch=0, lastDraw=0, lastLog=0;
// Start on PSU controls; return to monitoring after 20 seconds without interaction.
uint8_t page=0;
uint32_t MONITOR_RETURN_MS=20000;
uint16_t BACKGROUND=0x10c4,lcdAccent=0x07ff;
uint32_t lcdRefreshMs=1000,lcdAnimationMs=80;
String lcdTitles[3]={"BC250 PSU CONTROL","BC250 LIVE MONITOR","BC250 OS CONTROL"};
void invalidateDisplay();
volatile uint32_t metricsPollMs=5000;
volatile uint32_t usbMonitorAt=0;
volatile bool usbMonitorReceived=false;
bool usbMonitorFresh(){return usbMonitorReceived&&millis()-usbMonitorAt<10000;}
uint32_t screenActivityAt=0;
bool bootWasDown=false;
bool displayAwake=true;
bool displayManuallyOff=false;
bool suppressWakeTouch=false;
uint32_t wakeTouchReleaseAt=0,wakeTouchGuardUntil=0;
constexpr uint32_t AUTO_CUT_DELAY_MS=15000;
bool autoCutPending=false;
uint32_t shutdownSignalAt=0;
char shutdownNonce[33]={};
void wakeDisplay();
bool otaActive=false,otaSucceeded=false,otaHashActive=false;
String otaError;
uint32_t otaRebootAt=0,lastAnimation=0;
mbedtls_sha256_context otaHash;
unsigned char otaExpected[32];
RelayRestart::AbiScanner otaRelayAbi;
constexpr gpio_num_t RELAY_PIN=GPIO_NUM_22;
RelayHoldGuard relay;
portMUX_TYPE relayLock=portMUX_INITIALIZER_UNLOCKED;
bool relayPolicyOK=false;
QueueHandle_t actions;
portMUX_TYPE stateLock=portMUX_INITIALIZER_UNLOCKED;
struct Telemetry {
    float cpu=NAN, cpuTemp=NAN, gpu=NAN, gpuTemp=NAN;
    float ramUsed=NAN, ramTotal=NAN, vramUsed=NAN, vramTotal=NAN;
};
struct State { bool online=false; uint32_t checked=0, uptime=0,lastGood=0;bool hasGood=false;uint32_t metricAt[8]={}; int code=0; char text[64]="Waiting for Wi-Fi"; char bootId[40]={}; Telemetry metrics; } state;

String currentNonce(){
    char value[33];portENTER_CRITICAL(&stateLock);memcpy(value,shutdownNonce,sizeof(value));portEXIT_CRITICAL(&stateLock);
    return String(value);
}
void renewShutdownNonce(){
    char value[33];for(int i=0;i<4;i++)snprintf(value+i*8,9,"%08lx",static_cast<unsigned long>(esp_random()));
    portENTER_CRITICAL(&stateLock);memcpy(shutdownNonce,value,sizeof(value));portEXIT_CRITICAL(&stateLock);
    autoCutPending=false;
}

void initializeRelay() {
    relayPolicyOK=relayHoldSelfTest();
    bool heldHigh=gpio_ll_is_digital_io_hold(&GPIO,RELAY_PIN)&&gpio_get_level(RELAY_PIN)==1;
    relayResumedAfterRestart=RelayRestart::consume(restartTicket,esp_reset_reason()==ESP_RST_SW,heldHigh)&&relayPolicyOK;
    int level=relayResumedAfterRestart?1:0;
    // Configure the matching output BEFORE releasing the pad hold. No LOW pulse
    // on a verified planned restart; every other boot still starts OFF.
    gpio_set_level(RELAY_PIN,level);
    gpio_config_t cfg={};cfg.pin_bit_mask=1ULL<<RELAY_PIN;
    cfg.mode=GPIO_MODE_INPUT_OUTPUT;
    cfg.pull_down_en=relayResumedAfterRestart?GPIO_PULLDOWN_DISABLE:GPIO_PULLDOWN_ENABLE;
    cfg.pull_up_en=GPIO_PULLUP_DISABLE;cfg.intr_type=GPIO_INTR_DISABLE;
    gpio_config(&cfg);gpio_set_level(RELAY_PIN,level);
    if(relayResumedAfterRestart)relay.on(millis());
    gpio_hold_dis(RELAY_PIN);
}
struct RelayState { bool high; uint32_t cooldown; };
RelayState relaySnapshot() {
    portENTER_CRITICAL(&relayLock);
    RelayState s={relay.high(),relay.cooldownRemaining(millis())};
    portEXIT_CRITICAL(&relayLock);return s;
}
void turnRelayOff() {
    if(padPowerPending)padPowerResult="cancelled_by_off";
    padPowerPending=false;usbMonitorReceived=false;
    portENTER_CRITICAL(&stateLock);state.online=false;state.hasGood=false;state.metrics=Telemetry{};portEXIT_CRITICAL(&stateLock);
    portENTER_CRITICAL(&relayLock);relay.off(millis());gpio_set_level(RELAY_PIN,0);gpio_hold_dis(RELAY_PIN);RelayRestart::clear(restartTicket);portEXIT_CRITICAL(&relayLock);
    consolePrintln("{\"event\":\"relay_off\"}");
    renewShutdownNonce();
    confirmation="";page=0;displayAwake=false;digitalWrite(21,LOW);
}
bool turnRelayOn() {
    relayOnError="";
    if(!relayPolicyOK){relayOnError="relay_self_test_failed";return false;}
    if(otaActive||otaRebootAt){relayOnError="firmware_update_active";return false;}
    bool wasHigh=relaySnapshot().high;
    if(!wasHigh&&!gamepadWake.stop()){relayOnError="bluetooth_stop_failed";return false;}
    portENTER_CRITICAL(&relayLock);
    bool accepted=relay.on(millis());
    if(accepted)gpio_set_level(RELAY_PIN,1);
    portEXIT_CRITICAL(&relayLock);
    if(!accepted)relayOnError="relay_cooldown";
    if(accepted)consolePrintln("{\"event\":\"relay_on\",\"mode\":\"hold_until_off_or_reset\"}");
    if(accepted&&!wasHigh){renewShutdownNonce();wakeDisplay();screenActivityAt=millis();}
    return accepted;
}

bool restartWithRelayState(){
    RelayRestart::clear(restartTicket);
    if(relaySnapshot().high){
        if(gpio_get_level(RELAY_PIN)!=1||gpio_hold_en(RELAY_PIN)!=ESP_OK){
            otaError="relay_hold_failed";otaRebootAt=0;return false;
        }
        RelayRestart::arm(restartTicket);
    }
    Serial.flush();
    ESP.restart();return true;
}

State snapshot() {
    portENTER_CRITICAL(&stateLock); State s=state; portEXIT_CRITICAL(&stateLock); return s;
}
void setState(bool online,int code,const char* text,uint32_t uptime=0,const Telemetry& metrics=Telemetry{},const char* bootId=nullptr) {
    portENTER_CRITICAL(&stateLock);
    state.online=online; state.code=code; state.uptime=uptime; state.checked=millis();
    if(online){
        const float Telemetry::* fields[]={&Telemetry::cpu,&Telemetry::cpuTemp,&Telemetry::gpu,&Telemetry::gpuTemp,&Telemetry::ramUsed,&Telemetry::ramTotal,&Telemetry::vramUsed,&Telemetry::vramTotal};
        float Telemetry::* writable[]={&Telemetry::cpu,&Telemetry::cpuTemp,&Telemetry::gpu,&Telemetry::gpuTemp,&Telemetry::ramUsed,&Telemetry::ramTotal,&Telemetry::vramUsed,&Telemetry::vramTotal};
        for(unsigned i=0;i<8;i++){
            if(isfinite(metrics.*fields[i])){state.metrics.*writable[i]=metrics.*fields[i];state.metricAt[i]=millis();}
            else if(millis()-state.metricAt[i]>=10000)state.metrics.*writable[i]=NAN;
        }
        state.lastGood=millis();state.hasGood=true;
    }
    if(bootId&&strlen(bootId)==36)snprintf(state.bootId,sizeof(state.bootId),"%s",bootId);
    snprintf(state.text,sizeof(state.text),"%s",text);
    portEXIT_CRITICAL(&stateLock);
}
bool recentMetrics(const State& s){return s.hasGood&&millis()-s.lastGood<10000;}
bool ready() {State s=snapshot();return configured&&(usbMonitorFresh()||WiFi.status()==WL_CONNECTED)&&s.online&&millis()-s.checked<10000;}
String statusJson() {
    State s=snapshot(); RelayState rs=relaySnapshot(); DynamicJsonDocument doc(4096);
    usbHci.status(doc.createNestedObject("usb_bluetooth"),usbHciEnabled);
    doc["firmware"]=FIRMWARE_VERSION; doc["wifi_connected"]=WiFi.status()==WL_CONNECTED;
    doc["ip"]=WiFi.localIP().toString(); doc["target"]=host; doc["configured"]=configured;
    doc["agent_online"]=s.online && millis()-s.checked<10000;
    doc["metrics_available"]=recentMetrics(s);doc["metrics_stale"]=recentMetrics(s)&&!doc["agent_online"].as<bool>();
    doc["metrics_age_seconds"]=s.hasGood?(millis()-s.lastGood)/1000:0;
    doc["metrics_transport"]=usbMonitorFresh()?"usb":"wifi";doc["metrics_stale_after_seconds"]=10;
    doc["state"]=s.text; doc["http_code"]=s.code; doc["uptime_seconds"]=s.uptime;
    doc["touch_calibrated"]=calibrated; doc["heap"]=ESP.getFreeHeap(); doc["ms"]=millis();
    doc["atx_control_mode"]="hold_with_final_shutdown_cutoff"; doc["automatic_atx_cutoff"]=true;
    doc["relay_boot_state"]=relayResumedAfterRestart?"RESUMED_PLANNED_ON":"OFF";
    doc["relay_restart_hold_supported"]=true;doc["relay_resumed_after_restart"]=relayResumedAfterRestart;
    doc["ota_keep_power_supported"]=true;doc["relay_restart_abi"]=RelayRestart::ABI;doc["ota_error"]=otaError;doc["relay_on_persisted"]=false;
    doc["relay_contacts_sensed"]=false;doc["relay_cooldown_ms"]=rs.cooldown;
    doc["relay_gpio"]=22;doc["relay_active_high"]=true;
    doc["relay_output"]=rs.high?"HIGH":"LOW";
    doc["relay_gpio_readback"]=gpio_get_level(RELAY_PIN);
    doc["relay_output_latch"]=(REG_READ(GPIO_OUT_REG)>>22)&1;
    doc["relay_output_enabled"]=(REG_READ(GPIO_ENABLE_REG)&(1UL<<22))!=0;
    doc["relay_policy_self_test_ok"]=relayPolicyOK;
    doc["display_awake"]=displayAwake;doc["display_manually_off"]=displayManuallyOff;
    doc["esp_restart_supported"]=true;doc["esp_restart_allowed"]=!autoCutPending&&!otaActive&&!otaRebootAt;doc["sd_ready"]=sdReady;doc["sd_error"]=sdError;doc["web_source"]=sdReady&&SD.exists("/bc250/index.html")?"sd":"firmware";
    doc["metrics_poll_ms"]=metricsPollMs;doc["auto_setup_supported"]=true;doc["auto_setup_saved"]=sdReady&&learnedPcName.length()>0;doc["agent_port"]=agentPort;
    doc["display_sleep_policy"]="relay_off";
    doc["display_page"]=page;doc["monitor_return_seconds"]=MONITOR_RETURN_MS/1000;
    doc["shutdown_nonce"]=currentNonce();doc["server_boot_id"]=s.bootId;
    doc["automatic_cut_pending"]=autoCutPending;
    doc["automatic_cut_delay_seconds"]=AUTO_CUT_DELAY_MS/1000;
    doc["wifi_mac"]=WiFi.macAddress();
    doc["ota_supported"]=true;doc["ota_in_progress"]=otaActive;
    doc["running_partition"]=esp_ota_get_running_partition()->label;
    doc["gamepad_wake_supported"]=true;
    bool fresh=recentMetrics(s);
    JsonObject metrics=doc.createNestedObject("metrics");
    const char* names[]={"cpu_percent","cpu_temp_c","gpu_percent","gpu_temp_c","ram_used_gib","ram_total_gib","vram_used_gib","vram_total_gib"};
    const float values[]={s.metrics.cpu,s.metrics.cpuTemp,s.metrics.gpu,s.metrics.gpuTemp,s.metrics.ramUsed,s.metrics.ramTotal,s.metrics.vramUsed,s.metrics.vramTotal};
    for(int i=0;i<8;i++){if(fresh&&isfinite(values[i]))metrics[names[i]]=values[i];else metrics[names[i]]=nullptr;}
    String result; serializeJson(doc,result); return result;
}
bool requestAction(const String& name) {
    if (!ready()||WiFi.status()!=WL_CONNECTED) return false;
    uint8_t value=name=="shutdown"?1:name=="reboot"?2:0;
    return value && xQueueSend(actions,&value,0)==pdTRUE;
}
void networkTask(void*) {
    uint32_t lastPoll=0;
    uint32_t lastReconnect=0;
    uint32_t lastWakeConfig=0;
    bool pending=false; uint32_t pendingAt=0;
    for (;;) {
        uint8_t action=0;
        if (WiFi.status()!=WL_CONNECTED || !configured) {
            if(configured&&millis()-lastReconnect>=10000){lastReconnect=millis();WiFi.reconnect();}
            if(!usbMonitorFresh())setState(false,0,configured?"Wi-Fi connecting":"Setup required");
            vTaskDelay(pdMS_TO_TICKS(500)); continue;
        }
        if(!host.length()){if(!usbMonitorFresh())setState(false,0,"Waiting for USB setup");vTaskDelay(pdMS_TO_TICKS(500));continue;}
        bool haveAction=xQueueReceive(actions,&action,0)==pdTRUE;
        if (haveAction || millis()-lastPoll>=(usbMonitorFresh()?30000:metricsPollMs) || lastPoll==0) {
            if (haveAction && pending) {
                consolePrintln("{\"event\":\"power_request_rejected\",\"reason\":\"busy_or_unavailable\"}");
            } else {
                WiFiClient client; HTTPClient http;
                String path=haveAction?(action==1?"shutdown":"reboot"):(usbMonitorFresh()?"status":"metrics");
                http.setConnectTimeout(1500); http.setTimeout(2500);
                http.begin(client,"http://"+host+":"+String(agentPort)+"/v1/"+path);
                http.addHeader("Authorization","Bearer "+token);
                if(!haveAction){http.addHeader("X-BC250-Panel-Nonce",currentNonce());http.addHeader("X-BC250-Panel-MAC",WiFi.macAddress());}
                int code;
                if (haveAction) { http.addHeader("Content-Type","application/json"); code=http.POST("{\"confirm\":true}"); }
                else code=http.GET();
                StaticJsonDocument<1536> doc;
                bool valid=false;
                if (code>0) { String body=http.getString(); valid=body.length()<=1400 && !deserializeJson(doc,body); }
                http.end(); lastPoll=millis();
                if (valid && doc["device"]=="bc250" && code==200 && !haveAction) {
                    const char* remote=doc["state"]|"unknown";
                    bool running=strcmp(remote,"running")==0;
                    if (pending && millis()-pendingAt>30000 && running) pending=false;
                    Telemetry m;
                    JsonObject values=doc["metrics"];
                    auto number=[&](const char* key,float divisor=1.0f)->float{
                        JsonVariant value=values[key];
                        return value.is<float>()?value.as<float>()/divisor:NAN;
                    };
                    m.cpu=number("cpu_percent");m.cpuTemp=number("cpu_temp_c");
                    m.gpu=number("gpu_percent");m.gpuTemp=number("gpu_temp_c");
                    constexpr float GiB=1073741824.0f;
                    m.ramUsed=number("ram_used_bytes",GiB);m.ramTotal=number("ram_total_bytes",GiB);
                    m.vramUsed=number("vram_used_bytes",GiB);m.vramTotal=number("vram_total_bytes",GiB);
                    if(!usbMonitorFresh())setState(running&&!pending,code,pending?"Power request sent":remote,doc["uptime_seconds"]|0UL,m,doc["boot_id"]|"");
                } else if (valid && doc["device"]=="bc250" && code==202 && haveAction) {
                    pending=true; pendingAt=millis();
                    setState(false,code,action==1?"Shutdown requested":"Reboot requested");
                    consolePrintf("{\"event\":\"power_request_accepted\",\"action\":\"%s\"}\n",path.c_str());
                } else {
                    if(!usbMonitorFresh())setState(false,code,code==401?"Agent key mismatch":"BC250 not responding");
                }
            }
        }
        if(ready()&&(lastWakeConfig==0||millis()-lastWakeConfig>=30000)){
            lastWakeConfig=millis();WiFiClient client;HTTPClient http;
            http.setConnectTimeout(700);http.setTimeout(1200);http.begin(client,"http://"+host+":"+String(agentPort)+"/v1/gamepads");
            http.addHeader("Authorization","Bearer "+token);
            if(http.GET()==200){
                String body=http.getString();
                if(body.length()>0&&body.length()<2048){WakeConfigMessage message;snprintf(message.json,sizeof(message.json),"%s",body.c_str());xQueueOverwrite(wakeConfigs,&message);}
            }
            http.end();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

#include "web_page.h"

bool authenticate() {
    if (configured && web.authenticate("bc250",pin.c_str())) return true;
    web.requestAuthentication(); return false;
}
#include "sd_content.h"

void abortOta(const char* reason){
    otaError=reason;otaActive=false;Update.abort();
    if(otaHashActive){mbedtls_sha256_free(&otaHash);otaHashActive=false;}
}
void uploadFirmware(){
    HTTPUpload& upload=web.upload();
    if(upload.status==UPLOAD_FILE_START){
        otaSucceeded=false;otaError="";otaRelayAbi.reset();
        if(!configured||!web.authenticate("bc250",pin.c_str())){otaError="unauthorized";return;}
        if(web.header("X-BC250-UI")!="1"){otaError="invalid_request";return;}
        if(otaRebootAt||autoCutPending){otaError="shutdown_or_restart_pending";return;}
        if(!gamepadWake.stop()){otaError="standby_radio_busy";return;}
        String checksum=web.header("X-Firmware-SHA256");
        if(checksum.length()!=64){otaError="checksum_required";return;}
        for(int i=0;i<64;i++)if(!isxdigit(static_cast<unsigned char>(checksum[i]))){otaError="invalid_checksum";return;}
        for(int i=0;i<32;i++)otaExpected[i]=strtoul(checksum.substring(i*2,i*2+2).c_str(),nullptr,16);
        if(!Update.begin(UPDATE_SIZE_UNKNOWN)){otaError="update_unavailable";return;}
        mbedtls_sha256_init(&otaHash);mbedtls_sha256_starts_ret(&otaHash,0);otaHashActive=true;otaActive=true;
    }else if(upload.status==UPLOAD_FILE_WRITE&&otaActive){
        otaRelayAbi.feed(upload.buf,upload.currentSize);
        mbedtls_sha256_update_ret(&otaHash,upload.buf,upload.currentSize);
        if(Update.write(upload.buf,upload.currentSize)!=upload.currentSize)abortOta("write_failed");
    }else if(upload.status==UPLOAD_FILE_END&&otaActive){
        unsigned char digest[32]={};mbedtls_sha256_finish_ret(&otaHash,digest);
        unsigned char difference=0;for(int i=0;i<32;i++)difference|=digest[i]^otaExpected[i];
        if(difference){abortOta("checksum_mismatch");return;}
        if(relaySnapshot().high&&!otaRelayAbi.compatible()){abortOta("firmware_cannot_keep_relay_power");return;}
        otaSucceeded=Update.end(true);otaActive=false;mbedtls_sha256_free(&otaHash);otaHashActive=false;
        if(!otaSucceeded)otaError="invalid_firmware";
    }else if(upload.status==UPLOAD_FILE_ABORTED&&otaActive)abortOta("upload_aborted");
}
void startWeb() {
    const char* headers[]={"X-BC250-UI","X-Firmware-SHA256"}; web.collectHeaders(headers,2);
    startSDWeb();
    web.on("/",HTTP_GET,[]{ if(authenticate()&&!serveSDFile("index.html"))web.send_P(200,"text/html; charset=utf-8",PAGE); });
    web.on("/api/status",HTTP_GET,[]{if(authenticate()) {web.sendHeader("Cache-Control","no-store");web.send(200,"application/json",statusJson());}});
    web.on("/api/monitor/settings",HTTP_POST,[]{
        if(!authenticate())return;
        if(web.header("X-BC250-UI")!="1"||web.arg("plain").length()>128||otaActive||otaRebootAt){web.send(409,"application/json","{}");return;}
        StaticJsonDocument<128> request;
        if(deserializeJson(request,web.arg("plain"))||!request["poll_ms"].is<unsigned>()){
            web.send(400,"application/json","{}");return;
        }
        unsigned interval=request["poll_ms"];
        if(interval!=1000&&interval!=2000&&interval!=3000&&interval!=5000){web.send(400,"application/json","{}");return;}
        if(!config.putUInt("metrics_ms",interval)){web.send(500,"application/json","{}");return;}
        metricsPollMs=interval;savePadSD();
        web.send(200,"application/json","{\"saved\":true,\"stale_after_seconds\":10}");
    });
    web.on("/api/usb-bluetooth",HTTP_POST,[]{
        if(!authenticate())return;
        if(web.header("X-BC250-UI")!="1"||web.arg("plain").length()>128){web.send(403,"application/json","{}");return;}
        if(relaySnapshot().high||ready()||autoCutPending||otaActive||otaRebootAt){
            web.send(409,"application/json","{\"error\":\"shutdown_pc_first\"}");return;
        }
        StaticJsonDocument<192> doc;
        if(deserializeJson(doc,web.arg("plain"))||!doc["enabled"].is<bool>()){web.send(400,"application/json","{}");return;}
        if(!config.putBool("usb_hci",doc["enabled"].as<bool>())){web.send(500,"application/json","{}");return;}
        web.send(202,"application/json","{\"saved\":true,\"restarting\":true}");
        otaRebootAt=millis()+700;
    });
    web.on("/api/display",HTTP_POST,[]{
        if(!authenticate())return;
        if(web.header("X-BC250-UI")!="1"||web.arg("plain").length()>128){web.send(403,"application/json","{}");return;}
        StaticJsonDocument<192> doc;
        if(deserializeJson(doc,web.arg("plain"))||!doc["awake"].is<bool>()){web.send(400,"application/json","{}");return;}
        if(doc["awake"].as<bool>())wakeDisplay();
        else{displayManuallyOff=true;displayAwake=false;confirmation="";digitalWrite(21,LOW);}
        web.send(200,"application/json",displayAwake?"{\"display_awake\":true}":"{\"display_awake\":false}");
    });
    web.on("/api/gamepads",HTTP_GET,[]{if(authenticate()){
        DynamicJsonDocument doc(3072);gamepadWake.status(doc.to<JsonObject>());padPowerStatus(doc.to<JsonObject>());doc["restart_after_shutdown"]=wakeConfigNeedsRestart;
        doc["standby_blocked_by"]=gamepadWakeDisabled?"disabled":WiFi.status()!=WL_CONNECTED?"wifi_offline":relaySnapshot().high?"relay_on":ready()?"host_online":autoCutPending?"shutdown_pending":otaActive||otaRebootAt?"update":wakeConfigNeedsRestart?"identity_restart_required":padPowerPending?"power_request_pending":"";
        doc["disabled"]=gamepadWakeDisabled;doc["manual"]=gamepadManual;
        String body;serializeJson(doc,body);web.sendHeader("Cache-Control","no-store");web.send(200,"application/json",body);
    }});
    web.on("/api/gamepads/config",HTTP_POST,[]{
        if(!authenticate())return;
        if(web.header("X-BC250-UI")!="1"){web.send(403,"application/json","{}");return;}
        if(otaActive||otaRebootAt){web.send(409,"application/json","{}");return;}
        DynamicJsonDocument input(3072);
        if(web.arg("plain").length()>2048||deserializeJson(input,web.arg("plain"))||!input["manual"].is<bool>()){
            web.send(400,"application/json","{\"error\":\"invalid_request\"}");return;
        }
        if(!input["manual"].as<bool>()){
            if(!config.putBool("pad_manual",false)){web.send(500,"application/json","{}");return;}
            gamepadManual=false;savePadSD();web.send(200,"application/json","{\"saved\":true}");return;
        }
        DynamicJsonDocument next(2048);
        deserializeJson(next,gamepadConfig);
        uint8_t address[6];
        if(!input["host_mac"].is<const char*>()||!GamepadProtocol::address(input["host_mac"],address)){
            web.send(400,"application/json","{\"error\":\"invalid_host_mac\"}");return;
        }
        JsonArray addresses=input["addresses"];
        JsonArray peers=next["controllers"];
        if(addresses.isNull()||addresses.size()!=peers.size()){
            web.send(400,"application/json","{\"error\":\"invalid_controllers\"}");return;
        }
        for(unsigned i=0;i<addresses.size();i++){
            if(!addresses[i].is<const char*>()||!GamepadProtocol::address(addresses[i],address)){
                web.send(400,"application/json","{\"error\":\"invalid_controller_mac\"}");return;
            }
            // An IRK belongs to the old peer; never transfer it to a different controller.
            if(!peers[i]["address"].as<String>().equalsIgnoreCase(addresses[i].as<String>()))peers[i].remove("irk");
            peers[i]["address"]=addresses[i];
        }
        next["host_mac"]=input["host_mac"];
        String saved;serializeJson(next,saved);
        if(config.putString("pad_cfg",saved)!=saved.length()||!config.putBool("pad_manual",true)){
            web.send(500,"application/json","{\"error\":\"save_failed\"}");return;
        }
        gamepadWake.stop();
        gamepadWake.configure(next.as<JsonObject>());
        gamepadConfig=saved;gamepadManual=true;savePadSD();
        wakeConfigNeedsRestart=gamepadWake.needsIdentityRestart();
        // configure() changes the target identity; compare it to the actual radio too.
        uint8_t actual[6];esp_read_mac(actual,ESP_MAC_BT);
        GamepadProtocol::address(input["host_mac"],address);
        wakeConfigNeedsRestart=wakeConfigNeedsRestart||memcmp(actual,address,6)!=0;
        web.send(200,"application/json",String("{\"saved\":true,\"restart_after_shutdown\":")+(wakeConfigNeedsRestart?"true":"false")+"}");
    });
    web.on("/api/gamepads/enabled",HTTP_POST,[]{
        if(!authenticate())return;
        if(web.header("X-BC250-UI")!="1"||web.arg("plain").length()>128){web.send(403,"application/json","{}");return;}
        StaticJsonDocument<192> doc;
        if(deserializeJson(doc,web.arg("plain"))||!doc["enabled"].is<bool>()){web.send(400,"application/json","{}");return;}
        gamepadWakeDisabled=!doc["enabled"].as<bool>();config.putBool("pad_disabled",gamepadWakeDisabled);
        if(gamepadWakeDisabled)gamepadWake.stop();
        savePadSD();
        web.send(200,"application/json","{\"saved\":true}");
    });
    web.on("/api/power",HTTP_POST,[]{
        if(!authenticate()) return;
        if(web.header("X-BC250-UI")!="1" || web.arg("plain").length()>128) {web.send(403,"application/json","{\"error\":\"invalid_request\"}");return;}
        StaticJsonDocument<256> doc;
        if(deserializeJson(doc,web.arg("plain")) || !doc["confirm"].is<bool>() || !doc["confirm"].as<bool>()) {web.send(400,"application/json","{\"error\":\"confirmation_required\"}");return;}
        bool ok=requestAction(doc["action"]|"");
        web.send(ok?202:409,"application/json",ok?"{\"status\":\"queued\"}":"{\"error\":\"unavailable\"}");
    });
    web.on("/api/relay",HTTP_POST,[]{
        if(!authenticate())return;
        if(web.header("X-BC250-UI")!="1"||web.arg("plain").length()>128){web.send(403,"application/json","{\"error\":\"invalid_request\"}");return;}
        StaticJsonDocument<256> doc;
        if(deserializeJson(doc,web.arg("plain"))||!doc["confirm"].is<bool>()||!doc["confirm"].as<bool>()){web.send(400,"application/json","{\"error\":\"confirmation_required\"}");return;}
        String action=doc["action"]|"";
        if(action=="off"){
            if(!doc["force"].is<bool>()||!doc["force"].as<bool>()){web.send(400,"application/json","{\"error\":\"force_confirmation_required\"}");return;}
            turnRelayOff();web.send(200,"application/json","{\"status\":\"off\"}");
        }
        else if(action=="on"){bool ok=turnRelayOn();web.send(ok?202:409,"application/json",ok?"{\"status\":\"holding_on\"}":"{\"error\":\"cooldown_or_self_test_failed\"}");}
        else web.send(400,"application/json","{\"error\":\"invalid_action\"}");
    });
    web.on("/api/esp/restart",HTTP_POST,[]{
        if(!authenticate())return;
        if(web.header("X-BC250-UI")!="1"||web.arg("plain").length()>128){web.send(403,"application/json","{\"error\":\"invalid_request\"}");return;}
        StaticJsonDocument<256> doc;
        if(deserializeJson(doc,web.arg("plain"))||!doc["confirm"].is<bool>()||!doc["confirm"].as<bool>()){web.send(400,"application/json","{\"error\":\"confirmation_required\"}");return;}
        if(autoCutPending||otaActive||otaRebootAt){web.send(409,"application/json","{\"error\":\"busy\"}");return;}
        config.putBool("sleep_boot",true);
        web.sendHeader("Connection","close");web.send(202,"application/json","{\"restarting\":true}");otaRebootAt=millis()+1500;
    });
    web.on("/api/update",HTTP_POST,[]{
        if(!authenticate())return;
        if(otaSucceeded){web.sendHeader("Connection","close");web.send(200,"application/json","{\"updated\":true,\"restarting\":true}");otaRebootAt=millis()+1500;}
        else web.send(otaError=="shutdown_server_first"?409:400,"application/json","{\"error\":\""+otaError+"\"}");
    },uploadFirmware);
    // startSDWeb() already installed the authenticated SD asset fallback.
    // Replacing it here made separate CSS/JS files return 404 in 5.5.
    web.begin();
}


struct TextCache { String value; int y=-1,font=0; uint16_t color=0; } rows[16];
uint16_t buttonColors[2]={0xffff,0xffff};
String buttonLabels[2];
String drawnScreen;
void invalidateDisplay(){drawnScreen="";}
String metricKeys[6],centerStatus;
void text(const String& value,int y,uint16_t color=TFT_WHITE,int font=2) {
    TextCache* cache=nullptr;
    for(auto& row:rows){if(row.y==y){cache=&row;break;}if(row.y<0&&!cache)cache=&row;}
    if(cache&&cache->y==y&&cache->value==value&&cache->color==color&&cache->font==font)return;
    if(cache){cache->value=value;cache->y=y;cache->color=color;cache->font=font;}
    // Compose each changed row off-screen, then transfer it once. No erase flash.
    if(lineSprite.created()){
        lineSprite.fillSprite(BACKGROUND);lineSprite.setTextColor(color,BACKGROUND);
        lineSprite.drawString(value,0,0,font);lineSprite.pushSprite(12,y,0,0,296,font==1?8:20);
    }else{
        lcd.setTextPadding(296);lcd.setTextColor(color,BACKGROUND);lcd.drawString(value,12,y,font);lcd.setTextPadding(0);
    }
}
void button(int x,const char* label,uint16_t color) {
    int i=x==12?0:1;
    if(buttonLabels[i]==label&&buttonColors[i]==color)return;
    buttonLabels[i]=label;buttonColors[i]=color;
    lcd.fillRoundRect(x,139,142,49,8,color);
    lcd.setTextColor(TFT_WHITE,color);lcd.drawCentreString(label,x+71,153,2);
}
String valueText(float value,const char* suffix,int decimals=0) {
    return isfinite(value)?String(value,decimals)+suffix:String("--")+suffix;
}
void monitorMetric(int index,int x,int y,const char* label,const String& value,const String& detail,uint16_t color){
    String key=String(label)+value+detail+String(color);
    if(metricKeys[index]==key)return;
    metricKeys[index]=key;
    if(!metricSprite.created())return;
    metricSprite.fillSprite(BACKGROUND);
    metricSprite.setTextSize(1);metricSprite.setTextColor(TFT_LIGHTGREY,BACKGROUND);
    metricSprite.drawCentreString(label,52,0,2);
    metricSprite.setTextSize(2);metricSprite.setTextColor(color,BACKGROUND);
    metricSprite.drawCentreString(value,52,18,2);
    metricSprite.setTextSize(1);metricSprite.setTextColor(TFT_LIGHTGREY,BACKGROUND);
    metricSprite.drawCentreString(detail,52,54,1);
    metricSprite.pushSprite(x,y);
}
void drawMonitorAnimation(){
    if(!displayAwake||page!=1||confirmation.length()||!animationSprite.created())return;
    bool online=ready();uint16_t color=autoCutPending?TFT_YELLOW:online?lcdAccent:0x528a;
    animationSprite.fillSprite(BACKGROUND);
    animationSprite.drawCircle(36,36,31,color);
    float phase=(online||autoCutPending)?(millis()%1800)*6.2831853f/1800:0;
    for(int i=0;i<4;i++){
        float a=phase+i*1.5707963f;
        animationSprite.fillTriangle(36+cosf(a)*8,36+sinf(a)*8,
            36+cosf(a+0.35f)*26,36+sinf(a+0.35f)*26,
            36+cosf(a+1.05f)*23,36+sinf(a+1.05f)*23,color);
    }
    animationSprite.fillCircle(36,36,5,color);
    animationSprite.pushSprite(124,84);
}
void drawMonitor(const State& s){
    bool online=ready();Telemetry m=recentMetrics(s)?s.metrics:Telemetry{};
    uint16_t dim=0x738e;
    monitorMetric(0,2,6,"CPU",valueText(m.cpu,"%"),"LOAD",online?0x3edf:dim);
    monitorMetric(1,214,6,"GPU",valueText(m.gpu,"%"),isfinite(m.gpu)?"LOAD":"UNAVAILABLE",online?0xd57f:dim);
    monitorMetric(2,2,86,"CPU TEMP",valueText(m.cpuTemp,"C",1),"TEMPERATURE",online?0xfdae:dim);
    monitorMetric(3,214,86,"GPU TEMP",valueText(m.gpuTemp,"C",1),"TEMPERATURE",online?0xfdae:dim);
    monitorMetric(4,2,166,"RAM",valueText(m.ramUsed,"G",1),valueText(m.ramTotal," GiB total",1),online?0x6f5b:dim);
    monitorMetric(5,214,166,"VRAM",valueText(m.vramUsed,"G",1),valueText(m.vramTotal," GiB total",1),online?0x9e7f:dim);
    String status=autoCutPending?"STOPPING":online?(usbMonitorFresh()?"USB LIVE":"WI-FI"):recentMetrics(s)?"DELAY":"OFFLINE";
    if(status!=centerStatus){
        centerStatus=status;lcd.setTextColor(autoCutPending?TFT_YELLOW:online?TFT_GREEN:TFT_LIGHTGREY,BACKGROUND);
        lcd.setTextPadding(88);lcd.drawCentreString(status,160,28,2);lcd.setTextPadding(0);
        lcd.setTextColor(TFT_LIGHTGREY,BACKGROUND);lcd.drawCentreString("TAP FAN",160,190,1);
    }
    drawMonitorAnimation();
}
void nextPage(){page=(page+1)%3;confirmation="";screenActivityAt=millis();}
void draw() {
    if(!displayAwake)return;
    State s=snapshot();
    String screen=String(page)+":"+confirmation;
    if(screen!=drawnScreen){
        drawnScreen=screen;lcd.fillScreen(BACKGROUND);
        for(auto& row:rows){row.y=-1;row.value="";}
        for(int i=0;i<2;i++){buttonLabels[i]="";buttonColors[i]=0xffff;}
        for(auto& key:metricKeys)key="";centerStatus="";
    }
    if(page==1&&!confirmation.length()){drawMonitor(s);return;}
    text(lcdTitles[page],8,lcdAccent,2);
    if(confirmation.length()) {
        text(confirmation=="relay-on"?"Turn PSU ON and hold?":confirmation=="relay-off"?"CUT POWER IMMEDIATELY?":confirmation=="shutdown"?"Shut down BC250?":"Restart BC250?",58,TFT_YELLOW);
        text(confirmation=="relay-off"?"No OS shutdown. Unsaved data may be lost.":confirmation=="relay-on"?"Holds ON while BC250 is running.":confirmation=="shutdown"?"Auto PSU OFF after Linux final shutdown.":"Reboot keeps PSU ON. Save work first.",95,TFT_WHITE,1);
        button(12,"CONFIRM",0x8aa6); button(166,"CANCEL",0x328c);
    } else if(page==0) {
        RelayState rs=relaySnapshot();
        text(rs.high?"PSU REQUEST: ON / HOLD":"PSU REQUEST: OFF",43,rs.high?TFT_GREEN:TFT_YELLOW);
        text("Actual PSU / BC250 state: not sensed",73,TFT_WHITE,1);
        text(rs.cooldown?"Wait "+String((rs.cooldown+999)/1000)+" sec before ON":"ON remains active without network",91,TFT_LIGHTGREY,1);
        text("CUT POWER is not a normal shutdown",111,TFT_LIGHTGREY,1);
        button(12,"POWER ON",!rs.high&&!rs.cooldown&&relayPolicyOK?0x328c:0x3186);button(166,"CUT POWER",rs.high?0x9b68:0x3186);
    } else if(page==1) {
        bool online=ready();Telemetry m=recentMetrics(s)?s.metrics:Telemetry{};
        text("CPU   "+valueText(m.cpu,"%")+"    "+valueText(m.cpuTemp," C",1),42,TFT_WHITE);
        text("GPU   "+valueText(m.gpu,"%")+"    "+valueText(m.gpuTemp," C",1),66,TFT_WHITE);
        text("RAM   "+valueText(m.ramUsed,"",1)+" / "+valueText(m.ramTotal," GiB",1),90,TFT_WHITE);
        text("VRAM  "+valueText(m.vramUsed,"",1)+" / "+valueText(m.vramTotal," GiB",1),114,TFT_WHITE);
        text(autoCutPending?"Linux final shutdown / auto power OFF":online?String(usbMonitorFresh()?"USB / ":"Wi-Fi / ")+String(metricsPollMs/1000)+" sec":recentMetrics(s)?"DELAY / last sample < 10 sec":"OFFLINE / no data for 10 sec",145,online&&!autoCutPending?TFT_GREEN:TFT_YELLOW,1);
        text(online?"Uptime "+String(s.uptime/3600)+"h "+String((s.uptime/60)%60)+"m":"Waiting for fresh data",166,TFT_LIGHTGREY,1);
        text("Target: "+host,184,TFT_LIGHTGREY,1);
    } else {
        text(!configured?"SETUP REQUIRED":WiFi.status()!=WL_CONNECTED?"WI-FI CONNECTING":ready()?"BC250 CONNECTED":"BC250 NO RESPONSE",43,ready()?TFT_GREEN:TFT_YELLOW);
        text(String(s.text).substring(0,35),73,TFT_WHITE,1);
        text("Target: "+host,91,TFT_LIGHTGREY,1);
        text(ready()?"Up: "+String(s.uptime/60)+" min":"Power / network / agent unchecked",111,TFT_LIGHTGREY,1);
        button(12,"SHUT DOWN",ready()?0x9b68:0x3186); button(166,"RESTART",ready()?0x328c:0x3186);
    }
    text("Web: "+WiFi.localIP().toString(),201,TFT_WHITE,1);
    text("User: bc250  PIN: "+(configured?pin:"--"),216,TFT_LIGHTGREY,1);
    text("BOOT / title: Monitor > OS > PSU",230,TFT_LIGHTGREY,1);
}
void wakeDisplay(){
    displayManuallyOff=false;
    if(!displayAwake){page=relaySnapshot().high?2:0;confirmation="";screenActivityAt=millis();displayAwake=true;suppressWakeTouch=true;wakeTouchReleaseAt=0;wakeTouchGuardUntil=millis()+350;draw();digitalWrite(21,HIGH);}
}
void pollAutomaticPoweroff(){
    int size=shutdownUDP.parsePacket();
    if(size>0){
        char packet[384];IPAddress expected;expected.fromString(host);
        int n=shutdownUDP.read(packet,sizeof(packet)-1);packet[n>0?n:0]=0;
        StaticJsonDocument<512> doc;
        State s=snapshot();
        if(size<static_cast<int>(sizeof(packet))&&n==size&&shutdownUDP.remoteIP()==expected&&relaySnapshot().high&&
           !deserializeJson(doc,packet)&&doc["type"]=="bc250-poweroff-v1"&&
           String(doc["boot_id"]|"")==s.bootId&&strlen(s.bootId)==36&&String(doc["nonce"]|"")==currentNonce()){
            String message="bc250-poweroff-v1|"+String(s.bootId)+"|"+currentNonce();
            unsigned char digest[32]={};char signature[65];
            int result=mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                reinterpret_cast<const unsigned char*>(token.c_str()),token.length(),
                reinterpret_cast<const unsigned char*>(message.c_str()),message.length(),digest);
            for(int i=0;i<32;i++)snprintf(signature+i*2,3,"%02x",digest[i]);
            const char* supplied=doc["signature"]|"";unsigned char difference=0;
            if(result==0&&strlen(supplied)==64){
                for(int i=0;i<64;i++)difference|=signature[i]^supplied[i];
                if(!difference&&!autoCutPending){autoCutPending=true;shutdownSignalAt=millis();consolePrintln("{\"event\":\"final_shutdown_signal_accepted\"}");}
            }
        }
    }
    if(!autoCutPending)return;
    State s=snapshot();
    // A fresh running reply after the final signal cancels the cut, rather than guessing.
    if(s.online&&static_cast<int32_t>(s.checked-shutdownSignalAt)>0){renewShutdownNonce();return;}
    if(millis()-shutdownSignalAt>=AUTO_CUT_DELAY_MS){
        turnRelayOff();displayAwake=false;digitalWrite(21,LOW);
        gamepadWake.suppressFor(20000);
        consolePrintln("{\"event\":\"automatic_poweroff\",\"display_asleep\":true}");
        if(wakeConfigNeedsRestart){config.putBool("sleep_boot",true);Serial.flush();delay(100);restartWithRelayState();}
    }
}
void pollTouch() {
    if(millis()-lastTouch<20) return;
    lastTouch=millis(); bool down=touch.touched(); TS_Point p;
    if(down) {p=touch.getPoint();down=p.z>=300;}
    if(suppressWakeTouch){
        if(down)wakeTouchReleaseAt=0;
        else if(!wakeTouchReleaseAt)wakeTouchReleaseAt=millis();
        if(!down&&wakeTouchReleaseAt&&millis()-wakeTouchReleaseAt>=150&&static_cast<int32_t>(millis()-wakeTouchGuardUntil)>=0)suppressWakeTouch=false;
        downBefore=down;return;
    }
    bool pressed=down&&!downBefore;downBefore=down;
    if(!pressed)return;
    // The first press only wakes the backlight; require release before any action.
    if(!displayAwake){wakeDisplay();return;}
    screenActivityAt=millis();
    if(!calibrated)return;
    int x=lroundf(cal.ax*p.x+cal.bx*p.y+cal.cx),y=lroundf(cal.ay*p.x+cal.by*p.y+cal.cy);
    if(page==1&&!confirmation.length()){
        if(x>=116&&x<204&&y>=76&&y<164){nextPage();draw();}
        return;
    }
    if(x>=0&&x<320&&y>=0&&y<32){nextPage();draw();return;}
    if(y<139 || y>=188)return;
    bool left=x>=12&&x<154,right=x>=166&&x<308;
    if(!left&&!right)return;
    if(confirmation.length()) { if(left){if(confirmation=="relay-on")turnRelayOn();else if(confirmation=="relay-off")turnRelayOff();else requestAction(confirmation);} confirmation=""; }
    else if(page==0){RelayState rs=relaySnapshot();if(right&&rs.high){confirmation="relay-off";confirmationAt=millis();}else if(left&&!rs.high&&!rs.cooldown&&relayPolicyOK){confirmation="relay-on";confirmationAt=millis();}}
    else if(ready()) { confirmation=left?"shutdown":"reboot";confirmationAt=millis(); }
    draw();
}
void loadConfig() {
    ssid=config.getString("ssid","");password=config.getString("wifi_pw","");
    token=config.getString("token","");host=config.getString("host","");pin=config.getString("pin","");
    agentPort=config.getUShort("agent_port",9091);
    configured=ssid.length()>0&&token.length()==64&&pin.length()>=6;
}
void serialCommand(const String& line) {
    if(line=="status") {consolePrintln(statusJson());return;}
    if(line=="gamepad-status"){
        DynamicJsonDocument doc(3072);gamepadWake.status(doc.to<JsonObject>());doc["disabled"]=gamepadWakeDisabled;
        if(!usbHciEnabled){serializeJson(doc,Serial);Serial.println();}return;
    }
    if(line=="gamepad-disable"){
        gamepadWakeDisabled=true;config.putBool("pad_disabled",true);gamepadWake.stop();
        WiFi.reconnect();consolePrintln("{\"event\":\"gamepad_wake_disabled\"}");return;
    }
    if(line=="gamepad-enable"){
        gamepadWakeDisabled=false;config.putBool("pad_disabled",false);
        consolePrintln("{\"event\":\"gamepad_wake_enabled\"}");return;
    }
    if(line=="relay-off"||line=="relay-disarm"){turnRelayOff();return;}
    if(line=="relay-self-test"){consolePrintf("{\"event\":\"relay_policy_test\",\"pass\":%s,\"physical_output_changed\":false}\n",relayHoldSelfTest()?"true":"false");return;}
    StaticJsonDocument<512> doc;
    if(deserializeJson(doc,line)){consolePrintln("{\"error\":\"invalid_command\"}");return;}
    if(doc["command"]=="relay-set"){
        bool confirmed=doc["confirm"].is<bool>()&&doc["confirm"].as<bool>();
        String action=doc["action"]|"";
        bool force=doc["force"].is<bool>()&&doc["force"].as<bool>();
        if(!confirmed||(action!="on"&&action!="off")||(action=="off"&&!force)){consolePrintln("{\"error\":\"invalid_relay_request\"}");return;}
        bool ok=true;if(action=="on")ok=turnRelayOn();else turnRelayOff();
        consolePrintf("{\"event\":\"relay_set\",\"success\":%s}\n",ok?"true":"false");return;
    }
    if(doc["command"]!="provision"){consolePrintln("{\"error\":\"invalid_command\"}");return;}
    if(relaySnapshot().high){consolePrintln("{\"error\":\"turn_relay_off_before_configuration\"}");return;}
    String a=doc["ssid"]|"",b=doc["password"]|"",c=doc["token"]|"",d=doc["host"]|"",e=doc["pin"]|"";
    IPAddress ip;bool hex=c.length()==64;for(char ch:c)hex=hex&&(isxdigit(static_cast<unsigned char>(ch))!=0);
    if(!a.length()||a.length()>32||b.length()>63||!hex||(d.length()&&!ip.fromString(d))||e.length()<6||e.length()>16) {consolePrintln("{\"error\":\"invalid_configuration\"}");return;}
    bool ok=config.putString("ssid",a)>0&&config.putString("wifi_pw",b)>0&&config.putString("token",c)>0&&(config.putString("host",d)>0||!d.length())&&config.putString("pin",e)>0;
    consolePrintf("{\"event\":\"provisioned\",\"success\":%s}\n",ok?"true":"false");
    if(ok) {Serial.flush();delay(100);restartWithRelayState();}
}
void setup() {
    initializeRelay();
    renewShutdownNonce();
    config.begin("bc250-pm",false);loadConfig();
    usbHciEnabled=config.getBool("usb_hci",configured);
    metricsPollMs=config.getUInt("metrics_ms",5000);
    if(metricsPollMs<1000||metricsPollMs>5000)metricsPollMs=5000;
    if(usbHciEnabled){
        Serial.setRxBufferSize(4096);Serial.setTxBufferSize(2048);
        esp_log_level_set("*",ESP_LOG_NONE);
    }
    Serial.begin(usbHciEnabled?UsbHciBridge::BAUD:115200);delay(200);
    gamepadWakeDisabled=config.getBool("pad_disabled",false);
    gamepadManual=config.getBool("pad_manual",false);
    gamepadConfig=config.getString("pad_cfg",defaultGamepadConfig());
    mountSD();
    loadSDSettings(false);
    {
        DynamicJsonDocument pads(2048);
        if(!deserializeJson(pads,gamepadConfig)&&gamepadWake.configure(pads.as<JsonObject>()))gamepadWake.prepareIdentity();
    }
    // Reserve the contiguous controller memory before allocating LCD/Wi-Fi buffers.
    if(usbHciEnabled||!gamepadWakeDisabled)gamepadWake.reserveController();
    if(usbHciEnabled){usbHci.begin();if(relayResumedAfterRestart)usbHci.notifyHostRestart();}
    pinMode(0,INPUT_PULLUP);
    digitalWrite(21,LOW);pinMode(21,OUTPUT);lcd.init();lcd.setRotation(1);lcd.invertDisplay(false);digitalWrite(21,HIGH);
    lineSprite.setColorDepth(16);lineSprite.createSprite(296,22);
    metricSprite.setColorDepth(16);metricSprite.createSprite(104,64);
    animationSprite.setColorDepth(16);animationSprite.createSprite(72,72);
    touch.beginSoftware(25,39,32);touch.setRotation(0);
    Preferences saved;saved.begin("cyd-lcd-test",true);
    if(saved.getBytesLength("affine")==sizeof(cal)) {saved.getBytes("affine",&cal,sizeof(cal));calibrated=cal.magic==0x43594431&&isfinite(cal.ax)&&isfinite(cal.bx)&&isfinite(cal.cx)&&isfinite(cal.ay)&&isfinite(cal.by)&&isfinite(cal.cy);}
    saved.end();
    actions=xQueueCreate(1,sizeof(uint8_t));
    wakeConfigs=xQueueCreate(1,sizeof(WakeConfigMessage));
    WiFi.mode(WIFI_STA);WiFi.setHostname("bc250-panel");WiFi.setAutoReconnect(true);
    esp_wifi_stop();
    esp_err_t macResult=esp_wifi_set_mac(WIFI_IF_STA,gamepadWake.originalStationMac());
    esp_wifi_start();
    if(macResult!=ESP_OK)consolePrintf("{\"event\":\"wifi_mac_restore_failed\",\"code\":%d}\n",macResult);
    if(configured)WiFi.begin(ssid.c_str(),password.c_str());
    startWeb();shutdownUDP.begin(9092);xTaskCreatePinnedToCore(networkTask,"power-network",8192,nullptr,1,nullptr,0);
    consolePrintf("{\"event\":\"boot\",\"firmware\":\"%s\",\"relay_mode\":\"off\"}\n",FIRMWARE_VERSION);draw();
    if(config.getBool("sleep_boot",false)){config.remove("sleep_boot");displayAwake=false;digitalWrite(21,LOW);}
}
void loop() {
    web.handleClient();pollTouch();pollAutomaticPoweroff();
    if(ready()&&relaySnapshot().high&&!displayAwake&&!displayManuallyOff)wakeDisplay();
    WakeConfigMessage incoming;
    if(xQueueReceive(wakeConfigs,&incoming,0)==pdTRUE&&!gamepadManual&&gamepadConfig!=incoming.json){
        DynamicJsonDocument doc(2048);
        if(!deserializeJson(doc,incoming.json)&&doc["controllers"].is<JsonArray>()){
            String previousHost=gamepadWake.hostIdentity();
            if(gamepadWake.configure(doc.as<JsonObject>())){
                gamepadConfig=incoming.json;config.putString("pad_cfg",gamepadConfig);savePadSD();
                wakeConfigNeedsRestart=previousHost!=gamepadWake.hostIdentity()||gamepadWake.needsIdentityRestart();
            }
        }
    }
    if(usbHciEnabled){
        usbHci.setMonitorInterval(metricsPollMs);
        bool run=relaySnapshot().high&&!otaActive&&!otaRebootAt;
        if(run&&!usbHci.running())gamepadWake.stop();
        usbHci.poll(run);
    }
    UsbInfoProtocol::Info info;
    if(usbHciEnabled&&usbHci.takeInfo(info)){
        bool changed=false;
        if(RuntimeSettings::fillMissing(host,IPAddress(info.ip).toString())){
            agentPort=info.port;
            config.putString("host",host);config.putUShort("agent_port",agentPort);changed=true;
        }
        changed=RuntimeSettings::fillMissing(learnedPcName,String(info.hostname))||changed;
        if(!learnedBluetooth.length()){
            char mac[18];snprintf(mac,sizeof(mac),"%02X:%02X:%02X:%02X:%02X:%02X",info.bluetooth[0],info.bluetooth[1],info.bluetooth[2],info.bluetooth[3],info.bluetooth[4],info.bluetooth[5]);learnedBluetooth=mac;changed=true;
        }
        if(changed&&sdReady)savePadSD();
    }
    UsbMonitorProtocol::Sample monitor;
    if(usbHciEnabled&&usbHci.takeMonitor(monitor)&&relaySnapshot().high){
        Telemetry m;float* values[]={&m.cpu,&m.cpuTemp,&m.gpu,&m.gpuTemp,&m.ramUsed,&m.ramTotal,&m.vramUsed,&m.vramTotal};
        for(unsigned i=0;i<8;i++)*values[i]=monitor.values[i];
        char boot[37];snprintf(boot,sizeof(boot),"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",monitor.boot[0],monitor.boot[1],monitor.boot[2],monitor.boot[3],monitor.boot[4],monitor.boot[5],monitor.boot[6],monitor.boot[7],monitor.boot[8],monitor.boot[9],monitor.boot[10],monitor.boot[11],monitor.boot[12],monitor.boot[13],monitor.boot[14],monitor.boot[15]);
        usbMonitorAt=millis();usbMonitorReceived=true;
        setState(monitor.running,200,monitor.running?"running":"Power request sent",monitor.uptime,m,boot);
    }
    gamepadWake.poll(!usbHci.running()&&!padPowerPending&&!gamepadWakeDisabled&&(usbHciEnabled||WiFi.status()==WL_CONNECTED)&&!relaySnapshot().high&&!ready()&&!autoCutPending&&!otaActive&&!otaRebootAt&&!wakeConfigNeedsRestart);
    if(gamepadWake.takeWake()&&!padPowerPending){
        padPowerPending=true;padPowerStarted=millis();padPowerLastTry=millis()-250;
        padPowerAttempts=0;padPowerResult="pending";padPowerError="";
    }
    if(padPowerPending){
        if(gamepadWakeDisabled||wakeConfigNeedsRestart||autoCutPending||otaActive||otaRebootAt){
            padPowerPending=false;padPowerResult="cancelled";
        }else if(millis()-padPowerStarted>=4000){
            padPowerPending=false;padPowerResult="failed";
        }else if(millis()-padPowerLastTry>=250){
            padPowerLastTry=millis();padPowerAttempts++;
            bool accepted=turnRelayOn();padPowerError=relayOnError;
            if(accepted){padPowerPending=false;padPowerResult="relay_on_requested";padPowerAcceptedAt=millis();}
            consolePrintf("{\"event\":\"gamepad_power_on\",\"accepted\":%s,\"attempt\":%lu}\n",accepted?"true":"false",static_cast<unsigned long>(padPowerAttempts));
        }
    }
    bool bootDown=digitalRead(0)==LOW;
    if(bootDown&&!bootWasDown){
        bool wasAsleep=!displayAwake;wakeDisplay();
        if(!wasAsleep){nextPage();draw();}
    }
    bootWasDown=bootDown;
    while(!usbHciEnabled&&Serial.available()) {char ch=Serial.read();if(ch=='\n'){serialCommand(serialLine);serialLine="";}else if(ch!='\r'){if(serialLine.length()<511)serialLine+=ch;else serialLine="";}}
    if(confirmation.length()&&millis()-confirmationAt>15000)confirmation="";
    if(displayAwake&&page!=1&&millis()-screenActivityAt>=MONITOR_RETURN_MS){page=1;confirmation="";draw();}
    if(displayAwake&&millis()-lastDraw>lcdRefreshMs){lastDraw=millis();draw();}
    if(displayAwake&&page==1&&millis()-lastAnimation>=lcdAnimationMs){lastAnimation=millis();drawMonitorAnimation();}
    if(otaRebootAt&&static_cast<int32_t>(millis()-otaRebootAt)>=0){delay(100);restartWithRelayState();}
    if(millis()-lastLog>10000){lastLog=millis();consolePrintln(statusJson());}
    delay(3);
}
