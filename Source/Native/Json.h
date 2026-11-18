#pragma once
#include <string>
#include <map>
#include <vector>
#include <stdexcept>
#include <cstdint>
#include <cstdlib>
#include <limits>

// Small strict JSON codec. No external JSON library or scripting runtime is loaded.
struct Json {
    enum Type { Null, String, Number, Boolean, Object, Array } type=Null;
    std::string s; int64_t n=0; bool b=false;
    std::map<std::string,Json> object; std::vector<Json> array;
    Json()=default; Json(const char* v):type(String),s(v){} Json(std::string v):type(String),s(std::move(v)){}
    Json(int v):type(Number),n(v){} Json(int64_t v):type(Number),n(v){} Json(bool v):type(Boolean),b(v){}
    static Json obj() {Json v;v.type=Object;return v;} static Json arr(){Json v;v.type=Array;return v;}
    Json& operator[](const std::string& key){if(type==Null)type=Object;if(type!=Object)throw std::runtime_error("JSON object expected");return object[key];}
    const Json& at(const std::string& key)const{auto p=object.find(key);if(type!=Object||p==object.end())throw std::runtime_error("JSON field missing");return p->second;}
    std::string str()const{if(type!=String)throw std::runtime_error("JSON string expected");return s;}
    int64_t integer()const{if(type!=Number)throw std::runtime_error("JSON integer expected");return n;}
    bool boolean()const{if(type!=Boolean)throw std::runtime_error("JSON boolean expected");return b;}
    static void utf8(std::string& out,unsigned c){if(c<=0x7f)out+=(char)c;else if(c<=0x7ff){out+=(char)(0xc0|(c>>6));out+=(char)(0x80|(c&63));}else if(c<=0xffff){out+=(char)(0xe0|(c>>12));out+=(char)(0x80|((c>>6)&63));out+=(char)(0x80|(c&63));}else{out+=(char)(0xf0|(c>>18));out+=(char)(0x80|((c>>12)&63));out+=(char)(0x80|((c>>6)&63));out+=(char)(0x80|(c&63));}}
    static std::string quote(const std::string& s){std::string out="\"";const char* h="0123456789abcdef";for(unsigned char c:s){if(c=='"'||c=='\\'){out+='\\';out+=(char)c;}else if(c<32){out+="\\u00";out+=h[c>>4];out+=h[c&15];}else out+=(char)c;}return out+'"';}
    std::string dump()const{
        if(type==Null)return "null";if(type==String)return quote(s);if(type==Number)return std::to_string(n);if(type==Boolean)return b?"true":"false";
        std::string out=type==Object?"{":"[";bool first=true;
        if(type==Object){for(auto& p:object){if(!first)out+=',';first=false;out+=quote(p.first)+':'+p.second.dump();}}
        else{for(auto& v:array){if(!first)out+=',';first=false;out+=v.dump();}}
        return out+(type==Object?'}':']');
    }
    struct Parser {
        const std::string& text;size_t i=0;int depth=0;
        void ws(){while(i<text.size()&&(text[i]==' '||text[i]=='\r'||text[i]=='\n'||text[i]=='\t'))i++;}
        [[noreturn]] void fail(){throw std::runtime_error("Invalid JSON");}
        char take(){if(i>=text.size())fail();return text[i++];}
        unsigned hex(){unsigned v=0;for(int k=0;k<4;k++){char c=take();v<<=4;if(c>='0'&&c<='9')v+=c-'0';else if(c>='a'&&c<='f')v+=c-'a'+10;else if(c>='A'&&c<='F')v+=c-'A'+10;else fail();}return v;}
        std::string string(){if(take()!='"')fail();std::string out;while(true){unsigned char c=take();if(c=='"')break;if(c<32)fail();if(c!='\\'){out+=(char)c;continue;}char e=take();if(e=='"'||e=='\\'||e=='/')out+=e;else if(e=='b')out+='\b';else if(e=='f')out+='\f';else if(e=='n')out+='\n';else if(e=='r')out+='\r';else if(e=='t')out+='\t';else if(e=='u'){unsigned code=hex();if(code>=0xd800&&code<=0xdbff){if(take()!='\\'||take()!='u')fail();unsigned low=hex();if(low<0xdc00||low>0xdfff)fail();code=0x10000+((code-0xd800)<<10)+low-0xdc00;}else if(code>=0xdc00&&code<=0xdfff)fail();Json::utf8(out,code);}else fail();}return out;}
        Json value(){ws();if(++depth>16)fail();Json v;char c=i<text.size()?text[i]:0;
            if(c=='"')v=Json(string());
            else if(c=='{'||c=='['){i++;bool obj=c=='{';v=obj?Json::obj():Json::arr();ws();if(i<text.size()&&text[i]==(obj?'}':']'))i++;else while(true){std::string key;if(obj){ws();key=string();ws();if(take()!=':')fail();if(v.object.count(key))fail();}Json child=value();if(obj)v.object.emplace(std::move(key),std::move(child));else v.array.push_back(std::move(child));ws();char delimiter=take();if(delimiter==(obj?'}':']'))break;if(delimiter!=',')fail();}}
            else if(c=='t'||c=='f'||c=='n'){std::string literal=c=='t'?"true":c=='f'?"false":"null";if(text.compare(i,literal.size(),literal)!=0)fail();i+=literal.size();if(c!='n')v=Json(c=='t');}
            else if(c=='-'||(c>='0'&&c<='9')){size_t start=i;if(c=='-')i++;if(i>=text.size()||text[i]<'0'||text[i]>'9')fail();if(text[i]=='0')i++;else while(i<text.size()&&text[i]>='0'&&text[i]<='9')i++;std::string digits=text.substr(start,i-start);size_t used=0;try{v=Json((int64_t)std::stoll(digits,&used));}catch(...){fail();}if(used!=digits.size())fail();}
            else fail();depth--;return v;
        }
    };
    static Json parse(const std::string& text){if(text.size()>65536)throw std::runtime_error("JSON too large");Parser p{text};Json v=p.value();p.ws();if(p.i!=text.size())p.fail();return v;}
};
