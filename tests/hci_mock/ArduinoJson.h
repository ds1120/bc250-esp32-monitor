#pragma once
struct MockValue{template<class T>void operator=(const T&){};};
struct JsonObject{MockValue operator[](const char*){return {};}};
