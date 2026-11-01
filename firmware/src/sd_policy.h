#pragma once
#include <stddef.h>
#include <string.h>
namespace SDPolicy {
inline bool allowedName(const char* name){
    if(!name)return false;
    size_t n=strlen(name);if(!n||n>64||strstr(name,".."))return false;
    for(size_t i=0;i<n;i++){
        char c=name[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'))return false;
    }
    const char* ext=strrchr(name,'.');if(!ext)return false;
    const char* allowed[]={".html",".css",".js",".json",".png",".svg",".jpg",".ico"};
    for(const char* candidate:allowed)if(!strcmp(ext,candidate))return true;
    return false;
}
inline bool formatConfirmed(const char* expected,const char* submitted,const char* confirmation,unsigned long elapsed){
    return expected&&submitted&&confirmation&&strlen(expected)==32&&
        !strcmp(expected,submitted)&&!strcmp(confirmation,"FORMAT SD")&&elapsed<=60000;
}
inline bool validIntervals(unsigned seconds,unsigned poll){return seconds>=5&&seconds<=600&&poll>=1000&&poll<=30000;}
template<class FS,class Text> bool commit(FS& fs,const Text& path){
    Text backup=path+".bak";
    if(fs.exists(path)){
        if(fs.exists(backup)&&!fs.remove(backup))return false;
        if(!fs.rename(path,backup))return false;
    }
    if(!fs.rename(path+".tmp",path)){
        if(fs.exists(backup))fs.rename(backup,path);
        return false;
    }
    return true;
}
}
