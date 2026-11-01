#include "../firmware/src/sd_policy.h"
#include <cassert>
#include <map>
#include <string>
#include <cstdio>
struct FS {
    std::map<std::string,std::string> files;
    bool failNew=false,failBackup=false;
    bool exists(const std::string& p){return files.count(p);}
    bool remove(const std::string& p){if(failBackup&&p=="settings.json.bak")return false;return files.erase(p);}
    bool rename(const std::string& from,const std::string& to){
        if(failNew&&from=="settings.json.tmp")return false;
        if(!exists(from)||exists(to))return false;
        files[to]=files[from];files.erase(from);return true;
    }
};
int main(){
    for(auto name:{"index.html","app.js","settings.json","fan.png","theme.css"})assert(SDPolicy::allowedName(name));
    for(auto name:{"../index.html","/settings.json","x/../../token","settings.json.tmp","firmware.bin","file%2ehtml","","a..js","a.html?x","a\\x.js"})assert(!SDPolicy::allowedName(name));
    assert(!SDPolicy::allowedName(nullptr));
    assert(!SDPolicy::allowedName((std::string(65,'a')+".js").c_str()));
    assert(SDPolicy::validIntervals(5,1000));assert(SDPolicy::validIntervals(600,30000));
    assert(!SDPolicy::validIntervals(4,1000));assert(!SDPolicy::validIntervals(601,1000));
    assert(!SDPolicy::validIntervals(20,999));assert(!SDPolicy::validIntervals(20,30001));
    const char* token="0123456789abcdef0123456789abcdef";
    assert(SDPolicy::formatConfirmed(token,token,"FORMAT SD",60000));
    assert(!SDPolicy::formatConfirmed(token,token,"FORMAT SD",60001));
    assert(!SDPolicy::formatConfirmed(token,"wrong","FORMAT SD",0));
    assert(!SDPolicy::formatConfirmed(token,token,"format sd",0));
    assert(!SDPolicy::formatConfirmed("",token,"FORMAT SD",0));
    assert(!SDPolicy::formatConfirmed(token,token,nullptr,0));
    FS fs;fs.files={{"settings.json","old"},{"settings.json.tmp","new"}};
    assert(SDPolicy::commit(fs,std::string("settings.json")));assert(fs.files["settings.json"]=="new");assert(fs.files["settings.json.bak"]=="old");
    fs.files["settings.json.tmp"]="next";fs.failNew=true;
    assert(!SDPolicy::commit(fs,std::string("settings.json")));assert(fs.files["settings.json"]=="new");
    fs.failNew=false;fs.failBackup=true;fs.files["settings.json.bak"]="previous";
    assert(!SDPolicy::commit(fs,std::string("settings.json")));assert(fs.files["settings.json"]=="new");
    FS empty;empty.files["settings.json.tmp"]="first";assert(SDPolicy::commit(empty,std::string("settings.json")));
    puts("PASS: SD paths, settings bounds, replacement, rollback and failed backup protection");
}
