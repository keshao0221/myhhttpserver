#ifndef CPPJSON_H
#define CPPJSON_H


#include<string>
#include<vector>
#include<map>
#include<iostream>
enum JsonType{JSON_NULL,JSON_BOOL,JSON_NUM,JSON_STR,JSON_ARR,JSON_OBJ};

struct JsonValue{

    JsonType type=JSON_NULL;
    bool val_bool=false;
    double val_num=0;
    std::string val_str;
    std::vector<JsonValue> val_arr;
    std::map<std::string,JsonValue> val_obj;


    JsonValue()=default;
    JsonValue(const std::string&s): type(JSON_STR),val_str(s){}
    JsonValue(const char* s): type(JSON_STR),val_str(s){}

    JsonValue& operator[](const std::string& key){
        if(type==JSON_NULL)type=JSON_OBJ;
        return val_obj[key];
    }
};

void skip(const char*& p);
void ifexpect(const char*& p,char expect);
void print_value(const JsonValue& type,std::ostream& os=std::cout,int indent=0);
JsonValue prase_obj(const char*& p);
JsonValue prase_arr(const char*& p);
JsonValue prase_str(const char*& p);
JsonValue prase_bool(const char*& p);
JsonValue prase_null(const char*& p);
JsonValue prase_num(const char*& p);
JsonValue prase_value(const std::string& input);
JsonValue prase_value(const char*& p);

#endif