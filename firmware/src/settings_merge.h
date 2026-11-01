#pragma once
namespace RuntimeSettings {
template<class Text> bool fillMissing(Text& saved,const Text& detected){
 if(saved.length()||!detected.length())return false;
 saved=detected;return true;
}
}
