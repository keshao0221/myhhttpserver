#include"cppjson.h"
#include<cstdlib>

void skip(const char*& p){
    while(*p ==' '||*p=='\t'||*p=='\n'||*p=='\r')p++;
}

void ifexpect(const char*& p,char expect){
    skip(p);
    if(*p!=expect){
        std::cerr<<"error!"<<"\n";
        exit(1);
    }
    ++p;
}

void print_value(const JsonValue& val,std::ostream& os,int indent) {

    if (val.type == JSON_NULL) {
        os << "NULL";
    }
    else if (val.type == JSON_BOOL) {
        os << (val.val_bool ? "TRUE" : "FALSE");
    }
    else if (val.type == JSON_NUM) {
        os << val.val_num;
    }
    else if (val.type == JSON_STR) {
        os << "\"" << val.val_str << "\"";
    }
    else if (val.type == JSON_ARR) {
        os << "[";
        if (!val.val_arr.empty()) {
            for (size_t i = 0; i < val.val_arr.size(); ++i) {
                os << "\n" << std::string((indent + 1) * 2, ' ');
                print_value(val.val_arr[i],os,indent + 1);
                if (i != val.val_arr.size() - 1) {
                    os << ",";
                }
            }
            os << "\n" << std::string(indent * 2, ' ');
        }
        os << "]";
    }
    else if (val.type == JSON_OBJ) {
        os << "{";
        if (!val.val_obj.empty()) {
            size_t count = 0;
            for (const auto& pair : val.val_obj) {
                os << "\n" << std::string((indent + 1) * 2, ' ');
                os << "\"" << pair.first << "\": ";
                print_value(pair.second,os,indent + 1);
                if (++count != val.val_obj.size()) {
                    os << ",";
                }
            }
            os << "\n" << std::string(indent * 2, ' ');
        }
        os << "}";
    }
}

JsonValue prase_obj(const char*& p);
JsonValue prase_arr(const char*& p);
JsonValue prase_str(const char*& p);
JsonValue prase_bool(const char*& p);
JsonValue prase_null(const char*& p);
JsonValue prase_num(const char*& p);

JsonValue prase_value(const std::string& input){
    const char* p=input.c_str();
    return prase_value(p);
}

JsonValue prase_value(const char*& p){
    skip(p);
    switch(*p){
        case'{':return prase_obj(p);
        case'[':return prase_arr(p);
        case'"':return prase_str(p);
        case't':return prase_bool(p);
        case'f':return prase_bool(p);
        case'n':return prase_null(p);
        default:
            if(*p=='-'||(*p>='0'&&*p<='9'))
                return prase_num(p);
            else
                exit(1);
    }
}

JsonValue prase_obj(const char*& p){
    ifexpect(p,'{');
    JsonValue v;
    v.type=JSON_OBJ;

    if(*p=='}'){p++;return v;}

    while(1){
        JsonValue key=prase_str(p);
        ifexpect(p,':');
        skip(p);

        JsonValue value=prase_value(p);
        v.val_obj[key.val_str]=value;
        
        skip(p);
        if(*p==',')p++;
        else if(*p=='}'){p++;break;}
        else {
            std::cerr<<"error of obj!\n";
            exit(1);
        }
        skip(p);
    }
    return v;
}

JsonValue prase_arr(const char*& p){
    ifexpect(p,'[');
    JsonValue v;
    v.type=JSON_ARR;

    if(*p==']'){p++;return v;}

    while(1){
        JsonValue tmp=prase_value(p);
        v.val_arr.push_back(tmp);

        skip(p);
        if(*p==',')p++;
        else if(*p==']'){p++;break;}
        else{
            std::cerr<<"error of arr!\n";
            exit(1);
        }

        skip(p);
    }
    return v;
}

JsonValue prase_str(const char*& p){
    ifexpect(p,'"');
    std::string res;
    while(*p!='"'){res+=*p;p++;}
    ifexpect(p,'"');
    JsonValue v;
    v.type=JSON_STR;
    v.val_str=res;
    return v;
}

JsonValue prase_bool(const char*& p){
    JsonValue v;
    v.type=JSON_BOOL;
    if(*p=='t'){
        p++;
        ifexpect(p,'r');
        ifexpect(p,'u');
        ifexpect(p,'e');
        v.val_bool=true;
    }else{
        ifexpect(p,'f');
        ifexpect(p,'a');
        ifexpect(p,'l');
        ifexpect(p,'s');
        ifexpect(p,'e');
        v.val_bool=false;
    }
    return v;
}

JsonValue prase_null(const char*& p){
    ifexpect(p,'n');
    ifexpect(p,'u');
    ifexpect(p,'l');
    ifexpect(p,'l');
    JsonValue v;
    v.type=JSON_NULL;
    return v;
}

JsonValue prase_num(const char*& p){
    const char* start=p;
    if(*p=='-')p++;
    while(*p>='0'&&*p<='9')p++;
    if(*p=='.'){
        p++;
        while(*p>='0'&&*p<='9')p++;
    }
    double val=std::strtod(start,nullptr);
    JsonValue v;
    v.type=JSON_NUM;
    v.val_num=val;
    return v;
}