#include "App.h"
#include <winhttp.h>
#include <algorithm>

static bool Id(const std::string& s){if(s.size()!=32)return false;for(char c:s)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;return true;}
static std::string Aad(const std::string& client,const std::string& request,const std::string& direction){return "GuanYuan/v1\n"+g.id+'\n'+client+'\n'+request+'\n'+direction;}
static std::vector<unsigned char> Aes(const std::string& secret,std::vector<unsigned char> input,const std::vector<unsigned char>& iv,const std::string& aad,bool encrypt){
    auto raw=Unbase64(secret);if(raw.size()!=32||iv.size()!=12||input.size()>8192||(!encrypt&&input.size()<16))throw Failure{403,"加密消息无效"};
    BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_KEY_HANDLE key=nullptr;
    auto cleanup=[&]{if(key)BCryptDestroyKey(key);if(alg)BCryptCloseAlgorithmProvider(alg,0);SecureZeroMemory(raw.data(),raw.size());};
    try{
        if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_AES_ALGORITHM,nullptr,0)<0||BCryptSetProperty(alg,BCRYPT_CHAINING_MODE,(PUCHAR)BCRYPT_CHAIN_MODE_GCM,sizeof(BCRYPT_CHAIN_MODE_GCM),0)<0||BCryptGenerateSymmetricKey(alg,&key,nullptr,0,raw.data(),32,0)<0)throw Failure{500,"加密服务不可用"};
        std::vector<unsigned char> tag(16);if(!encrypt){std::copy(input.end()-16,input.end(),tag.begin());input.resize(input.size()-16);}
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;BCRYPT_INIT_AUTH_MODE_INFO(auth);auth.pbNonce=(PUCHAR)iv.data();auth.cbNonce=12;auth.pbAuthData=(PUCHAR)aad.data();auth.cbAuthData=(ULONG)aad.size();auth.pbTag=tag.data();auth.cbTag=16;
        std::vector<unsigned char> output(input.size()+16);ULONG n=0;
        NTSTATUS status=encrypt?BCryptEncrypt(key,input.data(),(ULONG)input.size(),&auth,nullptr,0,output.data(),(ULONG)output.size(),&n,0):BCryptDecrypt(key,input.data(),(ULONG)input.size(),&auth,nullptr,0,output.data(),(ULONG)output.size(),&n,0);
        if(status<0)throw Failure{403,"加密消息校验失败"};output.resize(n);if(encrypt)output.insert(output.end(),tag.begin(),tag.end());cleanup();return output;
    }catch(...){cleanup();throw;}
}
Json Seal(const std::string& secret,const std::string& client,const std::string& request,const std::string& direction,const std::string& plain){
    if(!Id(client)||!Id(request))throw Failure{400,"消息标识无效"};auto iv=RandomBytes(12);auto data=Aes(secret,{plain.begin(),plain.end()},iv,Aad(client,request,direction),true);
    Json j=Json::obj();j["clientId"]=client;j["requestId"]=request;j["iv"]=Base64(iv);j["data"]=Base64(data);return j;
}
std::string Open(const std::string& secret,const Json& envelope,const std::string& direction){
    std::string client=envelope.at("clientId").str(),request=envelope.at("requestId").str();if(!Id(client)||!Id(request))throw Failure{400,"消息标识无效"};
    auto plain=Aes(secret,Unbase64(envelope.at("data").str()),Unbase64(envelope.at("iv").str()),Aad(client,request,direction),false);return {(char*)plain.data(),plain.size()};
}
Json RemoteCommand(const Json& envelope){
    Guard guard(g.lock);std::string id=envelope.at("clientId").str(),rid=envelope.at("requestId").str(),secret;
    int clientVersion=0;for(auto&c:g.clients)if(c.id==id){secret=c.secret;clientVersion=c.securityVersion;}if(secret.empty())throw Failure{403,"未配对的手机"};
    Json response=Json::obj();
    try{
        Json payload=Json::parse(Open(secret,envelope,"request"));int64_t expires=payload.at("expires").integer(),now=UnixNow();
        if(clientVersion!=2)throw Failure{426,"请在电脑前重新核对身份指纹并配对"};
        auto version=payload.object.find("version");if(version==payload.object.end()||version->second.integer()!=2)throw Failure{426,"请更新手机和电脑至安全协议新版"};
        if(expires<now||expires>now+20)throw Failure{408,"请求时间校验失败，请更新两端并重新连接以校准时间"};
        const Json& command=payload.at("command");if(command.at("clientId").str()!=id||command.at("nonce").str()!=rid)throw Failure{403,"消息身份不匹配"};
        // No internet pairing or relay-configuration export. Both require LAN access.
        if(command.at("action").str()=="relayConfig")throw Failure{403,"请在同一 Wi-Fi 同步设置"};
        if(command.at("action").str()!="status"&&!Equal(payload.at("session").str(),g.session))throw Failure{409,"电脑已重新启动，请重新连接后操作"};
        if(command.at("action").str()!="status"&&command.at("action").str()!="cancel"&&command.at("delay").integer()<30)throw Failure{400,"远程电源操作至少保留 30 秒倒计时"};
        response["result"]=Command(command.dump());response["status"]=200;
    }catch(const Failure&f){Json error=Json::obj();error["ok"]=false;error["error"]=f.message;response["result"]=error;response["status"]=f.code;}
    catch(...){Json error=Json::obj();error["ok"]=false;error["error"]="消息格式无效";response["result"]=error;response["status"]=400;}
    // The hint is encrypted/authenticated and bound to this fresh request. It never
    // bypasses expiry or executes a rejected command; the phone may retry status only.
    response["serverTime"]=UnixNow();return Seal(secret,id,rid,"response",response.dump());
}
struct RelayAddress{std::wstring host,path;INTERNET_PORT port;bool secure;};
static RelayAddress Address(const std::string& url){
    if(url.empty()||url.size()>512)throw Failure{400,"请输入中转服务的 HTTPS 地址"};for(unsigned char c:url)if(c<=32||c>=127)throw Failure{400,"中转地址无效"};
    std::wstring w=Wide(url);URL_COMPONENTS u{};u.dwStructSize=sizeof(u);u.dwHostNameLength=u.dwUrlPathLength=u.dwExtraInfoLength=u.dwUserNameLength=u.dwPasswordLength=(DWORD)-1;
    if(!WinHttpCrackUrl(w.c_str(),0,0,&u)||!u.dwHostNameLength||u.dwUserNameLength||u.dwPasswordLength||u.dwExtraInfoLength)throw Failure{400,"中转地址不能带账号、参数或片段"};
    RelayAddress a{{u.lpszHostName,u.dwHostNameLength},{u.lpszUrlPath,u.dwUrlPathLength},u.nPort,u.nScheme==INTERNET_SCHEME_HTTPS};
    if(!a.secure&&!(g.demo&&u.nScheme==INTERNET_SCHEME_HTTP&&a.host==L"127.0.0.1"))throw Failure{400,"跨网络中转必须使用 HTTPS"};
    while(!a.path.empty()&&a.path.back()==L'/')a.path.pop_back();return a;
}
void ValidateRelay(const std::string& url,const std::string& key){if(url.empty()&&key.empty())return;Address(url);if(key.size()!=64||key.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos)throw Failure{400,"中转访问密钥应为 64 位十六进制字符"};}
static std::atomic<bool> running{false};
static HANDLE thread=nullptr,stopEvent=nullptr;
static SRWLOCK requestLock=SRWLOCK_INIT;
static HINTERNET currentRequest=nullptr,currentStream=nullptr;
struct HttpHandle{HINTERNET h=nullptr;~HttpHandle(){if(h)WinHttpCloseHandle(h);}};
static void Message(const std::string& s){{Guard guard(g.lock);g.relayMessage=s;}NotifyUi();}
static Failure HttpFailure(const std::string&stage,DWORD error=GetLastError()){
    std::string detail=error==ERROR_WINHTTP_NAME_NOT_RESOLVED?"域名解析失败":error==ERROR_WINHTTP_TIMEOUT?"网络超时":error==ERROR_WINHTTP_CANNOT_CONNECT?"网络不可达":error==ERROR_WINHTTP_SECURE_FAILURE?"HTTPS 证书校验失败":"Windows 网络请求失败";
    return {503,stage+"："+detail+"（"+std::to_string(error)+"）"};
}
static Json Post(HINTERNET connection,const RelayAddress& address,const std::string& key,const wchar_t* endpoint,const Json& body,bool ntfy=false){
    HINTERNET request=WinHttpOpenRequest(connection,L"POST",(address.path+endpoint).c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,address.secure?WINHTTP_FLAG_SECURE:0);
    if(!request)throw Failure{503,"无法建立中转请求"};
    AcquireSRWLockExclusive(&requestLock);if(!running){ReleaseSRWLockExclusive(&requestLock);WinHttpCloseHandle(request);throw Failure{503,"中转已停止"};}currentRequest=request;ReleaseSRWLockExclusive(&requestLock);
    auto cleanup=[&]{AcquireSRWLockExclusive(&requestLock);if(currentRequest==request){currentRequest=nullptr;WinHttpCloseHandle(request);}ReleaseSRWLockExclusive(&requestLock);};
    try{
        DWORD redirect=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect));WinHttpSetTimeouts(request,5000,5000,5000,25000);
        std::wstring headers=ntfy?L"Content-Type: text/plain; charset=utf-8\r\nCache: no\r\nFirebase: no\r\n":L"Content-Type: application/json\r\nAuthorization: Bearer "+Wide(key)+L"\r\n";std::string data=body.dump();
        if(ntfy&&data.size()>4096)throw Failure{413,"加密消息过大"};
        if(!WinHttpSendRequest(request,headers.c_str(),(DWORD)-1,(void*)data.data(),(DWORD)data.size(),(DWORD)data.size(),0)||!WinHttpReceiveResponse(request,nullptr))throw HttpFailure("发送中转消息失败");
        DWORD code=0,size=sizeof(code);if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&code,&size,WINHTTP_NO_HEADER_INDEX))throw Failure{503,"中转响应无效"};
        if(code!=200)throw Failure{(int)code,"HTTP "+std::to_string(code)+"，"+(code==429?"公共服务达到使用限额，请稍后再试":code==401||code==403?"中转拒绝访问":"中转暂不可用")};
        std::string result;char bytes[2048];DWORD n=0;while(true){if(!WinHttpReadData(request,bytes,sizeof(bytes),&n))throw Failure{503,"中转连接中断"};if(!n)break;if(result.size()+n>16384)throw Failure{413,"中转响应过大"};result.append(bytes,n);}
        cleanup();return Json::parse(result);
    }catch(...){cleanup();throw;}
}
std::string NtfyTopic(const std::string& key,const std::string& client){
    std::vector<unsigned char> raw;for(size_t i=0;i<key.size();i+=2)raw.push_back((unsigned char)std::stoul(key.substr(i,2),nullptr,16));
    std::string value=Sign(Base64(raw),client.empty()?"GuanYuan":client,0,"ntfy",client.empty()?"request":"response",0);for(char&c:value){if(c=='+')c='-';else if(c=='/')c='_';}while(!value.empty()&&value.back()=='=')value.pop_back();return (client.empty()?"gyq_":"gyr_")+value;
}
static void NtfyListen(HINTERNET connection,const RelayAddress& address,const std::string& key){
    std::wstring path=address.path+L"/"+Wide(NtfyTopic(key))+L"/raw";
    HINTERNET stream=WinHttpOpenRequest(connection,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,address.secure?WINHTTP_FLAG_SECURE:0);
    if(!stream)throw Failure{503,"无法连接公共中转"};
    AcquireSRWLockExclusive(&requestLock);if(!running){ReleaseSRWLockExclusive(&requestLock);WinHttpCloseHandle(stream);return;}currentStream=stream;ReleaseSRWLockExclusive(&requestLock);
    auto cleanup=[&]{AcquireSRWLockExclusive(&requestLock);if(currentStream==stream){currentStream=nullptr;WinHttpCloseHandle(stream);}ReleaseSRWLockExclusive(&requestLock);};
    try{
        DWORD redirect=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;WinHttpSetOption(stream,WINHTTP_OPTION_REDIRECT_POLICY,&redirect,sizeof(redirect));WinHttpSetTimeouts(stream,5000,5000,5000,85000);
        if(!WinHttpSendRequest(stream,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(stream,nullptr))throw HttpFailure("连接公共中转失败");
        DWORD code=0,size=sizeof(code);if(!WinHttpQueryHeaders(stream,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&code,&size,WINHTTP_NO_HEADER_INDEX))throw HttpFailure("读取中转状态失败");
        if(code!=200)throw Failure{503,"HTTP "+std::to_string(code)+"，"+(code==429?"公共服务达到限额":"公共中转拒绝连接")};
        Message("跨网络：免服务器模式已连接");std::string pending;char bytes[2048];DWORD n=0;
        while(running&&listening){
            DWORD available=0;if(!WinHttpQueryDataAvailable(stream,&available)||!available||!WinHttpReadData(stream,bytes,std::min(available,(DWORD)sizeof(bytes)),&n)||!n)throw Failure{503,"公共中转连接中断"};pending.append(bytes,n);
            if(pending.size()>16384)throw Failure{413,"中转数据过大"};size_t end;
            while((end=pending.find('\n'))!=std::string::npos){std::string line=pending.substr(0,end);pending.erase(0,end+1);if(line.empty())continue;
                Json envelope,answer;try{envelope=Json::parse(line);answer=RemoteCommand(envelope);}catch(...){continue;}
                try{std::wstring topic=L"/"+Wide(NtfyTopic(key,envelope.at("clientId").str()+envelope.at("requestId").str()));Post(connection,address,key,topic.c_str(),answer,true);Message("跨网络：免服务器模式已连接 · 已回复手机");}
                catch(const Failure&f){Message("跨网络：电脑已收到请求，回执失败 · "+f.message);}
            }
        }cleanup();
    }catch(...){cleanup();throw;}
}
static DWORD WINAPI RelayLoop(void*){
    std::string url,key,id,mode;{Guard guard(g.lock);url=g.relayUrl;key=g.relayKey;id=g.id;mode=g.relayMode;}
    int backoff=1000;
    while(running&&listening){
        try{
            RelayAddress address=Address(url);HttpHandle session{WinHttpOpen(L"GuanYuan/1.3.3",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};
            if(!session.h)throw Failure{503,"Windows HTTPS 服务不可用"};
            HttpHandle connection{WinHttpConnect(session.h,address.host.c_str(),address.port,0)};if(!connection.h)throw Failure{503,"无法连接中转"};
            if(mode=="ntfy"){NtfyListen(connection.h,address,key);continue;}
            Json poll=Json::obj();poll["id"]=id;Message("跨网络：正在连接…");
            while(running&&listening){
                Json response=Post(connection.h,address,key,L"/v1/poll",poll);backoff=1000;Message("跨网络：已连接");
                auto p=response.object.find("envelope");if(p==response.object.end()||p->second.type==Json::Null)continue;
                try{Json reply=Json::obj();reply["id"]=id;reply["envelope"]=RemoteCommand(p->second);Post(connection.h,address,key,L"/v1/reply",reply);}catch(const Failure&){/* Ignore unauthenticated/malformed envelopes; never execute them. */}
            }
        }catch(const Failure&f){if(running)Message("跨网络："+f.message);}catch(...){if(running)Message("跨网络：连接中断，稍后重连");}
        if(running&&WaitForSingleObject(stopEvent,backoff)==WAIT_OBJECT_0)break;backoff=std::min(backoff*2,30000);
    }return 0;
}
void StartRelay(){
    if(running||!listening)return;{Guard guard(g.lock);if(g.relayUrl.empty())return;ValidateRelay(g.relayUrl,g.relayKey);}
    stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);if(!stopEvent){Message("跨网络：无法启动连接");return;}running=true;
    thread=CreateThread(nullptr,256*1024,RelayLoop,nullptr,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);if(!thread){running=false;CloseHandle(stopEvent);stopEvent=nullptr;Message("跨网络：无法启动连接");}
}
void StopRelay(){
    running=false;if(stopEvent)SetEvent(stopEvent);AcquireSRWLockExclusive(&requestLock);if(currentRequest){WinHttpCloseHandle(currentRequest);currentRequest=nullptr;}if(currentStream){WinHttpCloseHandle(currentStream);currentStream=nullptr;}ReleaseSRWLockExclusive(&requestLock);
    if(thread){WaitForSingleObject(thread,INFINITE);CloseHandle(thread);thread=nullptr;}if(stopEvent){CloseHandle(stopEvent);stopEvent=nullptr;}
}
void ConfigureRelay(const std::string& url,const std::string& key,const std::string& mode){
    ValidateRelay(url,key);if(mode!="private"&&mode!="ntfy")throw Failure{400,"中转模式无效"};StopRelay();
    {Guard guard(g.lock);auto oldUrl=g.relayUrl,oldKey=g.relayKey,oldMode=g.relayMode;g.relayUrl=url;g.relayKey=key;g.relayMode=mode;try{SaveSettings();}catch(...){g.relayUrl=oldUrl;g.relayKey=oldKey;g.relayMode=oldMode;StartRelay();throw;}g.relayMessage=url.empty()?"跨网络：未设置":"跨网络：正在连接…";}
    StartRelay();NotifyUi();
}
