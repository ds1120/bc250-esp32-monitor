#pragma once
#include "sd_policy.h"
#include "sd_defaults_generated.h"
bool seedSDDefaults(bool overwrite=false);
#include <sd_diskio.h>
extern "C" {
#include <ff.h>
#include <diskio.h>
}
// SD content is optional. Authentication, power policy, Wi-Fi credentials and recovery
// remain in flash/NVS. Called only on the Arduino loop task (no concurrent SD access).
bool sdName(const String& name){return SDPolicy::allowedName(name.c_str());}
String sdPath(const String& name){return "/bc250/"+name;}
void mountSD(){
    pinMode(33,OUTPUT);digitalWrite(33,HIGH); // deselect touch before hardware SPI starts
    sdSPI.begin(18,19,23,5);
    sdReady=SD.begin(5,sdSPI,4000000);
    if(!sdReady){sdError="SD mount failed; using firmware defaults";return;}
    sdError="";
    if(!SD.exists("/bc250")&&!SD.mkdir("/bc250")){sdReady=false;sdError="SD directory unavailable";}
    if(sdReady&&!seedSDDefaults())sdError="Some SD defaults could not be written; firmware recovery available";
}
bool validSDSettings(JsonObject d){
    if(d["schema"]!=1)return false;
    if(!d["monitor_return_seconds"].is<unsigned>()||d["monitor_return_seconds"].as<unsigned>()<5||d["monitor_return_seconds"].as<unsigned>()>600)return false;
    if(!d["metrics_poll_ms"].is<unsigned>()||d["metrics_poll_ms"].as<unsigned>()<1000||d["metrics_poll_ms"].as<unsigned>()>30000)return false;
    if(!SDPolicy::validIntervals(d["monitor_return_seconds"],d["metrics_poll_ms"]))return false;
    if(!d["gamepad_enabled"].is<bool>()||!d["gamepad_manual"].is<bool>())return false;
    if(!d["network"].isNull()){
        JsonObject net=d["network"];if(net.isNull()||!net["agent_host"].is<const char*>()||!net["agent_port"].is<unsigned>())return false;
        String address=net["agent_host"].as<String>();IPAddress parsed;
        if((address.length()&&!parsed.fromString(address))||net["agent_port"].as<unsigned>()<1||net["agent_port"].as<unsigned>()>65535)return false;
    }
    if(!d["device"].isNull()){
        JsonObject dev=d["device"];if(dev.isNull())return false;
        if(!dev["pc_name"].isNull()&&(!dev["pc_name"].is<const char*>()||strlen(dev["pc_name"])>63))return false;
        if(!dev["bluetooth_address"].isNull()){
            uint8_t mac[6];const char* address=dev["bluetooth_address"]|"";
            if(strlen(address)&&!GamepadProtocol::address(address,mac))return false;
        }
    }
    if(!d["lcd"].isNull()){
        JsonObject lcd=d["lcd"];
        if(lcd.isNull()||!lcd["background_rgb565"].is<unsigned>()||lcd["background_rgb565"].as<unsigned>()>65535||
           !lcd["accent_rgb565"].is<unsigned>()||lcd["accent_rgb565"].as<unsigned>()>65535||
           !lcd["refresh_ms"].is<unsigned>()||lcd["refresh_ms"].as<unsigned>()<250||lcd["refresh_ms"].as<unsigned>()>10000||
           !lcd["animation_ms"].is<unsigned>()||lcd["animation_ms"].as<unsigned>()<50||lcd["animation_ms"].as<unsigned>()>1000)return false;
        JsonArray titles=lcd["titles"];if(titles.isNull()||titles.size()!=3)return false;
        for(JsonVariant title:titles){
            if(!title.is<const char*>())return false;
            const char* value=title;size_t n=strlen(value);if(!n||n>25)return false;
            for(size_t i=0;i<n;i++)if(value[i]<32||value[i]>126)return false;
        }
    }
    if(!d["gamepads"].isNull()){
        GamepadWake validator;
        if(!validator.configure(d["gamepads"].as<JsonObject>()))return false;
    }
    return true;
}
void applySDSettings(JsonObject d,bool live){
    if(d["network"].is<JsonObject>()){
        host=d["network"]["agent_host"].as<String>();agentPort=d["network"]["agent_port"];
        if(live){config.putString("host",host);config.putUShort("agent_port",agentPort);}
    }
    if(d["device"].is<JsonObject>()){
        learnedPcName=d["device"]["pc_name"]|"";learnedBluetooth=d["device"]["bluetooth_address"]|"";
    }
    MONITOR_RETURN_MS=d["monitor_return_seconds"].as<unsigned>()*1000;
    metricsPollMs=d["metrics_poll_ms"].as<unsigned>();
    gamepadManual=d["gamepad_manual"];gamepadWakeDisabled=!d["gamepad_enabled"].as<bool>();
    if(!d["gamepads"].isNull()){
        String next;serializeJson(d["gamepads"],next);
        if(next!=gamepadConfig){
            if(live)gamepadWake.stop();
            gamepadConfig=next;
            if(live){
                gamepadWake.configure(d["gamepads"].as<JsonObject>());
                uint8_t actual[6],wanted[6];esp_read_mac(actual,ESP_MAC_BT);
                GamepadProtocol::address(d["gamepads"]["host_mac"],wanted);
                wakeConfigNeedsRestart=memcmp(actual,wanted,6)!=0;
            }
        }
    }
    if(!d["lcd"].isNull()){
        JsonObject lcd=d["lcd"];BACKGROUND=lcd["background_rgb565"];lcdAccent=lcd["accent_rgb565"];
        lcdRefreshMs=lcd["refresh_ms"];lcdAnimationMs=lcd["animation_ms"];
        for(int i=0;i<3;i++)lcdTitles[i]=lcd["titles"][i].as<String>();
    }
    if(live)invalidateDisplay();
    if(live&&gamepadWakeDisabled)gamepadWake.stop();
}
bool readSDSettings(const String& path,DynamicJsonDocument& doc){
    File file=SD.open(path,FILE_READ);
    if(!file||file.size()>4096||file.isDirectory())return false;
    auto result=deserializeJson(doc,file);file.close();
    return !result&&validSDSettings(doc.as<JsonObject>());
}
bool loadSDSettings(bool live){
    if(!sdReady)return false;
    DynamicJsonDocument doc(6144);
    if(!readSDSettings(sdPath("settings.json"),doc)){
        // Recover the previous completed write after an interrupted replacement.
        doc.clear();
        if(!readSDSettings(sdPath("settings.json")+".bak",doc)){
            if(SD.exists(sdPath("settings.json")))sdError="Invalid SD settings; using current/default settings";
            return false;
        }
        sdError="Recovered SD settings backup";
    }
    applySDSettings(doc.as<JsonObject>(),live);return true;
}
void currentSDSettings(JsonObject doc){
    doc["schema"]=1;
    JsonObject net=doc.createNestedObject("network");net["agent_host"]=host;net["agent_port"]=agentPort;
    JsonObject dev=doc.createNestedObject("device");dev["pc_name"]=learnedPcName;dev["bluetooth_address"]=learnedBluetooth;
    dev["usb_baud"]=UsbHciBridge::BAUD;doc["monitor_return_seconds"]=MONITOR_RETURN_MS/1000;
    doc["metrics_poll_ms"]=static_cast<uint32_t>(metricsPollMs);
    doc["gamepad_enabled"]=!gamepadWakeDisabled;doc["gamepad_manual"]=gamepadManual;
    JsonObject lcd=doc.createNestedObject("lcd");lcd["background_rgb565"]=BACKGROUND;lcd["accent_rgb565"]=lcdAccent;
    lcd["refresh_ms"]=lcdRefreshMs;lcd["animation_ms"]=lcdAnimationMs;
    JsonArray titles=lcd.createNestedArray("titles");for(const auto& title:lcdTitles)titles.add(title);
    DynamicJsonDocument pads(2048);if(!deserializeJson(pads,gamepadConfig))doc["gamepads"].set(pads.as<JsonObject>());
}
bool commitSDFile(const String& path){return SDPolicy::commit(SD,path);}
void savePadSD(){
    if(!sdReady)return;
    DynamicJsonDocument doc(6144);currentSDSettings(doc.to<JsonObject>());
    String path=sdPath("settings.json");SD.remove(path+".tmp");
    File file=SD.open(path+".tmp",FILE_WRITE);
    if(!file){sdError="SD settings write failed; NVS retained";return;}
    size_t expected=measureJson(doc),written=serializeJson(doc,file);file.flush();file.close();
    if(written!=expected||!commitSDFile(path)){sdError="SD settings commit failed; NVS retained";return;}
    sdError="";
}
bool seedSDDefaults(bool overwrite){
    if(!sdReady)return false;
    bool ok=true;
    for(const auto& entry:SD_DEFAULT_FILES){
        String path=sdPath(entry.name);
        if(SD.exists(path)&&!overwrite)continue;
        // An interrupted settings/user file replacement must keep its backup.
        SD.remove(path+".tmp");File file=SD.open(path+".tmp",FILE_WRITE);
        bool written=file&&file.write(reinterpret_cast<const uint8_t*>(entry.data),entry.size)==entry.size;
        file.flush();file.close();
        if(!written||!commitSDFile(path)){ok=false;SD.remove(path+".tmp");}
    }
    if(!SD.exists(sdPath("settings.json"))&&!SD.exists(sdPath("settings.json")+".bak"))savePadSD();
    return ok&&(SD.exists(sdPath("settings.json"))||SD.exists(sdPath("settings.json")+".bak"));
}
bool serveSDFile(const String& name){
    if(!sdReady||!sdName(name)||name=="settings.json")return false;
    String path=sdPath(name);File file=SD.open(path,FILE_READ);
    if(!file||file.isDirectory()||!file.size()){file.close();return false;}
    const char* type=name.endsWith(".html")?"text/html; charset=utf-8":name.endsWith(".css")?"text/css":name.endsWith(".js")?"application/javascript":name.endsWith(".png")?"image/png":name.endsWith(".svg")?"image/svg+xml":name.endsWith(".jpg")?"image/jpeg":name.endsWith(".ico")?"image/x-icon":"application/json";
    web.sendHeader("Cache-Control","no-store");web.streamFile(file,type);file.close();return true;
}
File sdUpload;
String sdUploadName,sdUploadError;
size_t sdUploadBytes=0;
bool sdUploadOK=false;
void uploadSDFile(){
    auto& upload=web.upload();
    if(upload.status==UPLOAD_FILE_START){
        sdUploadOK=false;sdUploadError="";sdUploadBytes=0;sdUploadName=web.arg("name");
        if(!configured||!web.authenticate("bc250",pin.c_str())||web.header("X-BC250-UI")!="1"){sdUploadError="unauthorized";return;}
        if(!sdReady||otaActive||otaRebootAt||!sdName(sdUploadName)){sdUploadError="sd_unavailable_or_invalid_name";return;}
        String path=sdPath(sdUploadName)+".tmp";SD.remove(path);sdUpload=SD.open(path,FILE_WRITE);
        if(!sdUpload)sdUploadError="open_failed";
    }else if(upload.status==UPLOAD_FILE_WRITE&&sdUpload&&!sdUploadError.length()){
        sdUploadBytes+=upload.currentSize;
        size_t limit=sdUploadName=="settings.json"?4096:262144;
        if(sdUploadBytes>limit){sdUploadError="file_too_large";return;}
        if(sdUpload.write(upload.buf,upload.currentSize)!=upload.currentSize)sdUploadError="write_failed";
    }else if(upload.status==UPLOAD_FILE_END){
        if(!sdUpload)return;
        sdUpload.flush();sdUpload.close();
        if(!sdUploadBytes)sdUploadError="empty_file";
        DynamicJsonDocument doc(6144);
        if(!sdUploadError.length()&&sdUploadName=="settings.json"&&!readSDSettings(sdPath(sdUploadName)+".tmp",doc))sdUploadError="invalid_settings";
        if(!sdUploadError.length()){
            if(commitSDFile(sdPath(sdUploadName))){
                sdUploadOK=true;sdError="";
                if(sdUploadName=="settings.json")applySDSettings(doc.as<JsonObject>(),true);
            }else sdUploadError="commit_failed";
        }
        if(!sdUploadOK)SD.remove(sdPath(sdUploadName)+".tmp");
    }else if(upload.status==UPLOAD_FILE_ABORTED){
        sdUpload.close();if(sdName(sdUploadName))SD.remove(sdPath(sdUploadName)+".tmp");sdUploadError="aborted";
    }
}
// Raw access is confined to the dedicated SD SPI slot, never ESP32 flash.
String sdFormatToken;
uint32_t sdFormatIssuedAt=0,sdFormatSectors=0;
bool rawSD(bool format,uint32_t& sectors){
    sdUpload.close();SD.end();sdReady=false;
    sdSPI.begin(18,19,23,5);
    uint8_t drive=sdcard_init(5,&sdSPI,4000000);
    if(drive==0xff){sdError="SD hardware initialization failed";return false;}
    DWORD count=0;WORD sectorSize=0;
    bool ok=!(disk_initialize(drive)&(STA_NOINIT|STA_NODISK|STA_PROTECT))&&
        disk_ioctl(drive,GET_SECTOR_COUNT,&count)==RES_OK&&
        disk_ioctl(drive,GET_SECTOR_SIZE,&sectorSize)==RES_OK&&sectorSize==512&&count>=131072;
    if(ok&&format&&count!=sectors){ok=false;sdError="Card size changed; inspect again";}
    if(ok&&format){
        void* work=malloc(16384);
        if(!work){ok=false;sdError="Insufficient format memory";}
        else{
            char volume[4]={static_cast<char>('0'+drive),':',0,0};
            FATFS fs={};FRESULT result=f_mount(&fs,volume,0);
            if(result==FR_OK)result=f_mkfs(volume,FM_FAT32|FM_SFD,0,work,16384);
            f_mount(nullptr,volume,0);free(work);
            ok=result==FR_OK;
            if(!ok)sdError="FAT32 format failed: "+String(static_cast<int>(result));
            disk_ioctl(drive,CTRL_SYNC,nullptr);
        }
    }
    if(ok)sectors=count;
    else if(!sdError.length())sdError="SD unavailable, write-protected or unsupported size";
    sdcard_uninit(drive);
    String error=sdError;mountSD();if(!ok)sdError=error;
    return ok;
}
void startSDWeb(){
    web.on("/api/sd/format-info",HTTP_POST,[]{
        if(!authenticate())return;
        if(web.header("X-BC250-UI")!="1"||otaActive||otaRebootAt){web.send(409,"application/json","{}");return;}
        sdFormatToken="";sdError="";uint32_t sectors=0;
        if(!rawSD(false,sectors)){web.send(503,"application/json","{\"error\":\"card_unavailable\"}");return;}
        char token[33];for(int i=0;i<4;i++)snprintf(token+i*8,9,"%08lx",static_cast<unsigned long>(esp_random()));
        sdFormatToken=token;sdFormatIssuedAt=millis();sdFormatSectors=sectors;
        DynamicJsonDocument doc(256);doc["token"]=sdFormatToken;doc["bytes"]=static_cast<uint64_t>(sectors)*512;
        doc["target"]="ESP32 SD slot (CS GPIO5)";doc["expires_seconds"]=60;
        String body;serializeJson(doc,body);web.sendHeader("Cache-Control","no-store");web.send(200,"application/json",body);
    });
    web.on("/api/sd/format",HTTP_POST,[]{
        if(!authenticate())return;
        if(web.header("X-BC250-UI")!="1"||otaActive||otaRebootAt){web.send(409,"application/json","{}");return;}
        StaticJsonDocument<256> doc;
        if(web.arg("plain").length()>256||deserializeJson(doc,web.arg("plain"))||
           !SDPolicy::formatConfirmed(sdFormatToken.c_str(),doc["token"]|"",doc["confirm"]|"",millis()-sdFormatIssuedAt)){
            web.send(400,"application/json","{\"error\":\"confirmation_required_or_expired\"}");return;
        }
        sdFormatToken=""; // one use, consumed before any destructive I/O
        uint32_t sectors=sdFormatSectors;sdError="";
        bool ok=rawSD(true,sectors);
        // Preserve the currently active configuration on the newly formatted card.
        if(ok&&sdReady){savePadSD();ok=SD.exists(sdPath("settings.json"));}
        else ok=false;
        web.send(ok?200:500,"application/json",ok?"{\"formatted\":true,\"filesystem\":\"FAT32\"}":"{\"error\":\"format_or_remount_failed\"}");
    });
    web.on("/recovery",HTTP_GET,[]{if(authenticate())web.send_P(200,"text/html; charset=utf-8",PAGE);});
    web.on("/api/sd/mount",HTTP_POST,[]{
        if(!authenticate())return;
        if(web.header("X-BC250-UI")!="1"||otaActive||otaRebootAt){web.send(409,"application/json","{}");return;}
        SD.end();mountSD();
        if(sdReady)loadSDSettings(true);
        web.send(sdReady?200:503,"application/json",sdReady?"{\"ready\":true}":"{\"ready\":false}");
    });
    web.on("/api/sd/seed",HTTP_POST,[]{
        if(!authenticate())return;
        if(web.header("X-BC250-UI")!="1"||!sdReady||otaActive||otaRebootAt){web.send(409,"application/json","{}");return;}
        bool overwrite=false;
        if(web.arg("plain").length()){
            StaticJsonDocument<128> request;
            if(deserializeJson(request,web.arg("plain"))||!request["overwrite"].is<bool>()){
                web.send(400,"application/json","{}");return;
            }
            overwrite=request["overwrite"];
        }
        bool ok=seedSDDefaults(overwrite);
        web.send(ok?200:500,"application/json",ok?"{\"installed\":true,\"settings_preserved\":true}":"{\"error\":\"default_write_failed\"}");
    });
    web.on("/api/sd",HTTP_GET,[]{
        if(!authenticate())return;
        DynamicJsonDocument doc(6144);doc["ready"]=sdReady;doc["error"]=sdError;doc["defaults_embedded"]=true;doc["defaults_version"]=FIRMWARE_VERSION;
        currentSDSettings(doc.createNestedObject("settings"));
        JsonArray files=doc.createNestedArray("files");
        if(sdReady){File dir=SD.open("/bc250");File file=dir.openNextFile();unsigned count=0;
            while(file&&count++<24){String name=file.name();int slash=name.lastIndexOf('/');if(slash>=0)name=name.substring(slash+1);
                if(!file.isDirectory()&&sdName(name)){JsonObject f=files.createNestedObject();f["name"]=name;f["bytes"]=file.size();}
                file.close();file=dir.openNextFile();}dir.close();}
        String body;serializeJson(doc,body);web.sendHeader("Cache-Control","no-store");web.send(200,"application/json",body);
    });
    web.on("/api/sd/file",HTTP_POST,[]{
        if(!authenticate())return;
        web.send(sdUploadOK?200:400,"application/json",sdUploadOK?"{\"saved\":true}":"{\"error\":\""+sdUploadError+"\"}");
    },uploadSDFile);
    web.on("/api/sd/settings",HTTP_GET,[]{
        if(!authenticate())return;
        DynamicJsonDocument doc(6144);currentSDSettings(doc.to<JsonObject>());String body;serializeJson(doc,body);
        web.sendHeader("Cache-Control","no-store");web.send(200,"application/json",body);
    });
    web.onNotFound([]{
        if(!authenticate())return;
        String name=web.uri();if(name.startsWith("/"))name=name.substring(1);
        if(!serveSDFile(name))web.send(404,"text/plain","Not found. Built-in recovery: /recovery");
    });
}
