#include "App.h"
#include <wincrypt.h>
#include <sddl.h>
#include <shlobj.h>
#include <powrprof.h>
#include <algorithm>
#include <fstream>

State g;
std::wstring Wide(const std::string& s){if(s.empty())return {};int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),nullptr,0);if(!n)throw Failure{400,"文字编码无效"};std::wstring v(n,0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),(int)s.size(),&v[0],n);return v;}
std::string Utf8(const std::wstring& s){if(s.empty())return {};int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);std::string v(n,0);WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),&v[0],n,nullptr,nullptr);return v;}
int64_t UnixNow(){FILETIME t;GetSystemTimeAsFileTime(&t);ULARGE_INTEGER n;n.LowPart=t.dwLowDateTime;n.HighPart=t.dwHighDateTime;return (int64_t)(n.QuadPart/10000000ULL-11644473600ULL);}
std::vector<unsigned char> RandomBytes(size_t count){std::vector<unsigned char>b(count);if(BCryptGenRandom(nullptr,b.data(),(ULONG)b.size(),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw Failure{500,"无法生成安全随机数"};return b;}
std::string RandomHex(size_t count){auto b=RandomBytes(count);const char*h="0123456789abcdef";std::string s;for(auto c:b){s+=h[c>>4];s+=h[c&15];}return s;}
std::string Base64(const std::vector<unsigned char>& bytes){DWORD n=0;if(!CryptBinaryToStringA(bytes.data(),(DWORD)bytes.size(),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,nullptr,&n))throw Failure{500,"编码失败"};std::string s(n,0);if(!CryptBinaryToStringA(bytes.data(),(DWORD)bytes.size(),CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,&s[0],&n))throw Failure{500,"编码失败"};s.resize(n);return s;}
std::vector<unsigned char> Unbase64(const std::string& s){DWORD n=0;if(s.size()>8192||!CryptStringToBinaryA(s.data(),(DWORD)s.size(),CRYPT_STRING_BASE64,nullptr,&n,nullptr,nullptr))throw Failure{400,"编码无效"};std::vector<unsigned char>b(n);if(!CryptStringToBinaryA(s.data(),(DWORD)s.size(),CRYPT_STRING_BASE64,b.data(),&n,nullptr,nullptr))throw Failure{400,"编码无效"};b.resize(n);return b;}
bool Equal(const std::string&a,const std::string&b){size_t diff=a.size()^b.size();for(size_t i=0;i<a.size();i++)diff|=(unsigned char)a[i]^(i<b.size()?(unsigned char)b[i]:0);return diff==0;}
std::string Sign(const std::string&secret,const std::string&id,int64_t time,const std::string&nonce,const std::string&action,int delay){
    auto key=Unbase64(secret);if(key.size()!=32)throw Failure{403,"密钥无效"};
    std::string plain=id+'\n'+std::to_string(time)+'\n'+nonce+'\n'+action+'\n'+std::to_string(delay);
    BCRYPT_HASH_HANDLE hash=nullptr;std::vector<unsigned char>digest(32);
    if(BCryptCreateHash(g.hmacProvider,&hash,nullptr,0,key.data(),(ULONG)key.size(),0)<0)throw Failure{500,"认证不可用"};
    NTSTATUS status=BCryptHashData(hash,(PUCHAR)plain.data(),(ULONG)plain.size(),0);if(status>=0)status=BCryptFinishHash(hash,digest.data(),32,0);BCryptDestroyHash(hash);
    if(status<0)throw Failure{500,"认证失败"};return Base64(digest);
}
static bool HexId(const std::string&s){if(s.size()!=32)return false;for(char c:s)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;return true;}
std::string Fingerprint(const std::string& modulus,const std::string& exponent,const std::string& id){
    std::string input="GuanYuan/pair/v2\n"+id+'\n'+modulus+'\n'+exponent;BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;std::vector<unsigned char> digest(32);
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw Failure{500,"无法生成身份指纹"};
    NTSTATUS status=BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0);if(status>=0)status=BCryptHashData(hash,(PUCHAR)input.data(),(ULONG)input.size(),0);if(status>=0)status=BCryptFinishHash(hash,digest.data(),32,0);if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(alg,0);if(status<0)throw Failure{500,"无法生成身份指纹"};return Base64(digest);
}
std::wstring FingerprintLabel(){auto bytes=Unbase64(g.fingerprint);std::wstring text;const wchar_t* hex=L"0123456789ABCDEF";for(size_t i=0;i<10&&i<bytes.size();i++){if(i&&i%2==0)text+=L" ";text+=hex[bytes[i]>>4];text+=hex[bytes[i]&15];}return text;}
static void CryptoInit(){
    if(BCryptOpenAlgorithmProvider(&g.hmacProvider,BCRYPT_SHA256_ALGORITHM,nullptr,BCRYPT_ALG_HANDLE_HMAC_FLAG)<0||BCryptOpenAlgorithmProvider(&g.rsaProvider,BCRYPT_RSA_ALGORITHM,nullptr,0)<0||BCryptGenerateKeyPair(g.rsaProvider,&g.rsa,2048,0)<0||BCryptFinalizeKeyPair(g.rsa,0)<0)throw Failure{500,"Windows 加密服务不可用"};
    ULONG n=0;BCryptExportKey(g.rsa,nullptr,BCRYPT_RSAPUBLIC_BLOB,nullptr,0,&n,0);std::vector<unsigned char>blob(n);
    if(BCryptExportKey(g.rsa,nullptr,BCRYPT_RSAPUBLIC_BLOB,blob.data(),n,&n,0)<0)throw Failure{500,"无法导出配对公钥"};
    auto header=(BCRYPT_RSAKEY_BLOB*)blob.data();auto start=blob.begin()+sizeof(BCRYPT_RSAKEY_BLOB);
    g.exponent=Base64({start,start+header->cbPublicExp});g.modulus=Base64({start+header->cbPublicExp,start+header->cbPublicExp+header->cbModulus});
}
static std::vector<unsigned char> ReadFileBytes(const std::wstring&path){
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(file==INVALID_HANDLE_VALUE)throw Failure{500,"无法读取配对配置"};
    DWORD n=GetFileSize(file,nullptr),got=0;if(n>65536||n==INVALID_FILE_SIZE){CloseHandle(file);throw Failure{500,"配对配置过大"};}
    std::vector<unsigned char>data(n);bool ok=ReadFile(file,data.data(),n,&got,nullptr)&&got==n;CloseHandle(file);if(!ok)throw Failure{500,"读取配置失败"};return data;
}
static void ReadSettings(){
    if(g.memoryOnly)return;
    if(GetFileAttributesW(g.settingsPath.c_str())==INVALID_FILE_ATTRIBUTES){SaveSettings();return;}
    auto saved=ReadFileBytes(g.settingsPath);if(saved.size()<6||memcmp(saved.data(),"PP01",4)||saved[4]>1)throw Failure{500,"配对配置损坏，请备份后重新配对"};
    DATA_BLOB input{(DWORD)saved.size()-5,saved.data()+5},output{};
    if(!CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output))throw Failure{500,"无法解密配对配置"};
    std::string text((char*)output.pbData,output.cbData);SecureZeroMemory(output.pbData,output.cbData);LocalFree(output.pbData);
    Json config=Json::parse(text);std::string id=config.at("Id").str();if(!HexId(id)||config.at("Clients").type!=Json::Array||config.at("Clients").array.size()>16)throw Failure{500,"配对配置无效"};
    std::vector<Client> clients;for(auto&item:config.at("Clients").array){Client c{item.at("Id").str(),item.at("Name").str(),item.at("Secret").str()};auto version=item.object.find("SecurityVersion");if(version!=item.object.end())c.securityVersion=(int)version->second.integer();if(!HexId(c.id)||Unbase64(c.secret).size()!=32||(c.securityVersion!=1&&c.securityVersion!=2))throw Failure{500,"配对配置无效"};clients.push_back(std::move(c));}
    g.id=id;g.clients=std::move(clients);
    auto relay=config.object.find("RelayUrl");if(relay!=config.object.end()){g.relayUrl=relay->second.str();g.relayKey=config.at("RelayKey").str();ValidateRelay(g.relayUrl,g.relayKey);auto mode=config.object.find("RelayMode");if(mode!=config.object.end())g.relayMode=mode->second.str();if(g.relayMode!="private"&&g.relayMode!="ntfy")throw Failure{500,"中转配置无效"};}
}
void SaveSettings(){
    if(g.memoryOnly)return;
    Json config=Json::obj();config["Id"]=g.id;config["Clients"]=Json::arr();
    config["RelayUrl"]=g.relayUrl;config["RelayKey"]=g.relayKey;
    config["RelayMode"]=g.relayMode;
    for(auto&c:g.clients){Json item=Json::obj();item["Id"]=c.id;item["Name"]=c.name;item["Secret"]=c.secret;item["SecurityVersion"]=c.securityVersion;config["Clients"].array.push_back(std::move(item));}
    std::string plain=config.dump();DATA_BLOB input{(DWORD)plain.size(),(BYTE*)plain.data()},encrypted{};BYTE scope=0;
    if(!CryptProtectData(&input,L"PalmPower",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&encrypted)){if(!g.demo)throw Failure{500,"Windows 用户加密服务不可用，已停止保存密钥"};scope=1;if(!CryptProtectData(&input,L"PalmPower",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN|CRYPTPROTECT_LOCAL_MACHINE,&encrypted))throw Failure{500,"无法保护配对密钥"};}
    HANDLE token=nullptr;OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token);DWORD size=0;GetTokenInformation(token,TokenUser,nullptr,0,&size);std::vector<unsigned char>user(size);
    if(!GetTokenInformation(token,TokenUser,user.data(),size,&size)){CloseHandle(token);LocalFree(encrypted.pbData);throw Failure{500,"无法设置配置权限"};}CloseHandle(token);
    LPWSTR sid=nullptr;ConvertSidToStringSidW(((TOKEN_USER*)user.data())->User.Sid,&sid);std::wstring sddl=L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;"+std::wstring(sid)+L")";LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)){LocalFree(encrypted.pbData);throw Failure{500,"无法限制配置文件权限"};}
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};std::wstring temp=g.settingsPath+L".tmp";
    HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,&attributes,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);LocalFree(descriptor);
    bool ok=file!=INVALID_HANDLE_VALUE;DWORD written=0;BYTE prefix[5]={'P','P','0','1',scope};
    if(ok){ok=WriteFile(file,prefix,5,&written,nullptr)&&written==5&&WriteFile(file,encrypted.pbData,encrypted.cbData,&written,nullptr)&&written==encrypted.cbData;FlushFileBuffers(file);CloseHandle(file);}
    SecureZeroMemory(encrypted.pbData,encrypted.cbData);LocalFree(encrypted.pbData);
    if(!ok||!MoveFileExW(temp.c_str(),g.settingsPath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temp.c_str());throw Failure{500,"保存配对配置失败"};}
}
void RenewCode(){Guard guard(g.lock);auto bytes=RandomBytes(4);uint32_t value=0;memcpy(&value,bytes.data(),4);char text[7];snprintf(text,sizeof(text),"%06u",value%1000000);g.code=text;g.codeExpires=GetTickCount64()+300000;NotifyUi();}
void ClosePairing(){Guard guard(g.lock);g.code.clear();g.codeExpires=0;NotifyUi();}
static std::string Label(const std::string&a){return a=="shutdown"?"关机":a=="sleep"?"睡眠":"休眠";}
static void ExecuteWindows(const std::string&action){
    if(action=="shutdown"){
        wchar_t system[MAX_PATH];GetSystemDirectoryW(system,MAX_PATH);std::wstring exe=std::wstring(system)+L"\\shutdown.exe",cmd=L"\""+exe+L"\" /s /t 0";STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
        if(!CreateProcessW(exe.c_str(),&cmd[0],nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))throw Failure{500,"Windows 拒绝启动关机请求"};
        DWORD wait=WaitForSingleObject(pi.hProcess,5000),code=1;GetExitCodeProcess(pi.hProcess,&code);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
        if(wait!=WAIT_OBJECT_0||code!=0)throw Failure{500,"Windows 未接受关机请求，请查看电脑状态"};return;
    }
    if(action!="sleep"&&action!="hibernate")throw Failure{400,"不支持的操作"};
    HANDLE token=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token))throw Failure{500,"无法取得电源管理权限"};
    TOKEN_PRIVILEGES tp{},previous{};tp.PrivilegeCount=1;tp.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;LookupPrivilegeValueW(nullptr,SE_SHUTDOWN_NAME,&tp.Privileges[0].Luid);DWORD size=sizeof(previous);
    SetLastError(0);bool enabled=AdjustTokenPrivileges(token,FALSE,&tp,sizeof(previous),&previous,&size)&&GetLastError()==ERROR_SUCCESS;
    if(!enabled){CloseHandle(token);throw Failure{500,"当前用户没有电源管理权限"};}
    BOOLEAN ok=SetSuspendState(action=="hibernate",FALSE,FALSE);AdjustTokenPrivileges(token,FALSE,&previous,0,nullptr,nullptr);CloseHandle(token);
    if(!ok)throw Failure{500,"Windows 未接受睡眠或休眠请求"};
}
static void CALLBACK PowerDue(PTP_CALLBACK_INSTANCE,void*,PTP_TIMER){
    std::string action;{Guard guard(g.lock);if(g.action.empty()||g.executing||GetTickCount64()<g.deadline)return;action=g.action;g.action.clear();g.executing=true;g.message="正在请求 Windows "+Label(action);}
    NotifyUi();std::string message;
    try{if(!g.demo)ExecuteWindows(action);message=g.demo?"模拟完成："+Label(action):"已向 Windows 提交"+Label(action)+"请求";}catch(const Failure&f){message="执行失败："+f.message;}catch(...){message="电源操作发生错误";}
    {Guard guard(g.lock);g.message=message;g.executing=false;g.executed++;}NotifyUi();
}
void SchedulePower(const std::string&action,int delay){
    if(action!="shutdown"&&action!="sleep"&&action!="hibernate")throw Failure{400,"不支持的操作"};if(delay<10||delay>3600)throw Failure{400,"倒计时须为 10～3600 秒"};
    Guard guard(g.lock);if(!g.action.empty()||g.executing)throw Failure{409,"已有待执行操作，请先取消"};
    if(action=="sleep"&&!g.sleep)throw Failure{409,"这台电脑当前不支持睡眠"};if(action=="hibernate"&&!g.hibernate)throw Failure{409,"Windows 尚未启用休眠"};
    if(!g.powerTimer)g.powerTimer=CreateThreadpoolTimer(PowerDue,nullptr,nullptr);if(!g.powerTimer)throw Failure{500,"无法创建倒计时"};
    g.action=action;g.deadline=GetTickCount64()+(uint64_t)delay*1000;g.message="手机已安排"+Label(action);
    ULARGE_INTEGER due;due.QuadPart=(uint64_t)(-((int64_t)delay*10000000LL));FILETIME time{due.LowPart,due.HighPart};SetThreadpoolTimer(g.powerTimer,&time,0,0);NotifyUi();
}
void CancelPower(){Guard guard(g.lock);if(g.executing)throw Failure{409,"操作已经提交给 Windows"};if(!g.action.empty()){g.action.clear();g.message="已取消，电脑继续运行";}if(g.powerTimer)SetThreadpoolTimer(g.powerTimer,nullptr,0,0);NotifyUi();}
Json PowerStatus(){Guard guard(g.lock);uint64_t now=GetTickCount64();Json j=Json::obj();j["ok"]=true;j["name"]=g.name;j["session"]=g.session;j["simulated"]=g.demo;j["action"]=g.action;j["executing"]=g.executing;j["remaining"]=(int)(g.action.empty()?0:(g.deadline>now?(g.deadline-now+999)/1000:0));j["message"]=g.message;j["relayStatus"]=g.relayMessage;if(g.demo)j["mockExecutions"]=g.executed;return j;}
Json Info(){Guard guard(g.lock);Json j=Json::obj();j["app"]="PalmPower";j["version"]=1;j["pairVersion"]=2;j["securityVersion"]=2;j["id"]=g.id;j["name"]=g.name;j["port"]=boundPort;j["modulus"]=g.modulus;j["exponent"]=g.exponent;j["sleep"]=g.sleep;j["hibernate"]=g.hibernate;j["simulated"]=g.demo;j["relayCapable"]=true;return j;}
Json Pair(const std::string&body,const std::string&ip){
    Guard guard(g.lock);if(!listening)throw Failure{503,"服务已停止"};uint64_t now=GetTickCount64();
    if(now>=g.codeExpires)throw Failure{403,"请先在电脑点击「开始配对」，再核对身份指纹"};
    g.pairAttempts.erase(std::remove_if(g.pairAttempts.begin(),g.pairAttempts.end(),[&](uint64_t t){return now-t>60000;}),g.pairAttempts.end());if(g.pairAttempts.size()>=10)throw Failure{429,"配对尝试过多，请一分钟后再试"};g.pairAttempts.push_back(now);
    for(auto it=g.attempts.begin();it!=g.attempts.end();)if(it->second.empty()||now-it->second.back()>60000)it=g.attempts.erase(it);else ++it;
    if(g.attempts.size()>=128&&!g.attempts.count(ip))throw Failure{429,"请稍后再试"};auto&attempts=g.attempts[ip];attempts.erase(std::remove_if(attempts.begin(),attempts.end(),[&](uint64_t t){return now-t>60000;}),attempts.end());if(attempts.size()>=5)throw Failure{429,"尝试过多，请一分钟后再试"};attempts.push_back(now);
    Client client;try{
        Json outer=Json::parse(body);auto version=outer.object.find("version");if(version==outer.object.end()||version->second.integer()!=2)throw Failure{426,"请更新手机版，首次配对需要核对身份指纹"};
        auto encrypted=Unbase64(outer.at("data").str());if(encrypted.size()!=256)throw Failure{403,""};
        BCRYPT_OAEP_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM,nullptr,0};ULONG n=0;std::vector<unsigned char>plain(256);
        if(BCryptDecrypt(g.rsa,encrypted.data(),(ULONG)encrypted.size(),&padding,nullptr,0,plain.data(),(ULONG)plain.size(),&n,BCRYPT_PAD_OAEP)<0)throw Failure{403,""};
        Json data=Json::parse(std::string((char*)plain.data(),n));SecureZeroMemory(plain.data(),plain.size());
        if(!Equal(data.at("f").str(),g.fingerprint))throw Failure{403,""};
        if(now>=g.codeExpires||!Equal(data.at("p").str(),g.code))throw Failure{403,""};
        client={data.at("i").str(),data.at("n").str(),data.at("s").str(),2};if(!HexId(client.id)||client.name.size()>120||Unbase64(client.secret).size()!=32)throw Failure{403,""};
    }catch(const Failure& f){if(f.code==426)throw;throw Failure{403,"配对码、身份指纹错误或已过期，请查看电脑端"};}catch(...){throw Failure{403,"配对内容无效，请重新连接电脑"};}
    auto old=g.clients;auto existing=std::find_if(g.clients.begin(),g.clients.end(),[&](const Client&c){return c.id==client.id;});
    if(existing==g.clients.end()){if(g.clients.size()>=16)throw Failure{409,"配对设备已满，请清除旧配对"};g.clients.push_back(client);}else *existing=client;
    try{SaveSettings();}catch(...){g.clients=std::move(old);throw;}
    g.lastSeen=now;g.lastClient=client.name;ClosePairing();Json j=Json::obj();j["ok"]=true;j["id"]=g.id;NotifyUi();return j;
}
Json Command(const std::string&body){
    Json j;std::string id,nonce,action,signature;int64_t timestamp,delay;
    try{j=Json::parse(body);id=j.at("clientId").str();nonce=j.at("nonce").str();action=j.at("action").str();signature=j.at("signature").str();timestamp=j.at("timestamp").integer();delay=j.at("delay").integer();}catch(...){throw Failure{403,"请先配对"};}
    if(!HexId(id)||!HexId(nonce)||signature.size()!=44||delay<0||delay>3600)throw Failure{400,"请求无效"};
    if(action!="status"&&action!="cancel"&&action!="shutdown"&&action!="sleep"&&action!="hibernate"&&action!="relayConfig")throw Failure{400,"不支持的操作"};
    Guard guard(g.lock);if(!listening)throw Failure{503,"服务已停止"};
    auto client=std::find_if(g.clients.begin(),g.clients.end(),[&](const Client&c){return c.id==id;});if(client==g.clients.end()||!Equal(Sign(client->secret,id,timestamp,nonce,action,(int)delay),signature))throw Failure{403,"配对已失效，请重新配对"};
    if(client->securityVersion!=2)throw Failure{426,"请更新手机和电脑，重新核对身份指纹并配对"};
    int64_t utc=UnixNow();if(timestamp<utc-120||timestamp>utc+120)throw Failure{403,"手机与电脑时间相差过大，请开启自动校时"};uint64_t now=GetTickCount64();
    for(auto it=g.nonces.begin();it!=g.nonces.end();)if(now-it->second>300000)it=g.nonces.erase(it);else ++it;
    std::string unique=id+":"+nonce;if(g.nonces.count(unique))throw Failure{409,"重复请求已拒绝"};if(g.nonces.size()>=4096)throw Failure{429,"请求过多，请稍后再试"};g.nonces[unique]=now;
    g.lastSeen=now;g.lastClient=client->name;
    if(action=="relayConfig"){
        Json config=Json::obj();config["securityVersion"]=2;config["url"]=g.relayUrl;config["key"]=g.relayKey;config["mode"]=g.relayMode;config["name"]=g.name;config["sleep"]=g.sleep;config["hibernate"]=g.hibernate;config["simulated"]=g.demo;
        Json result=Json::obj();result["ok"]=true;result["envelope"]=Seal(client->secret,id,nonce,"config",config.dump());return result;
    }
    if(action=="status"){}else if(action=="cancel")CancelPower();else SchedulePower(action,(int)delay);
    NotifyUi();return PowerStatus();
}
void ClearClients(){
    bool rotate=false;{Guard guard(g.lock);if(g.executing)throw Failure{409,"操作已经提交给 Windows"};auto old=g.clients;auto key=g.relayKey;g.clients.clear();rotate=g.relayMode=="ntfy"&&!g.relayKey.empty();if(rotate)g.relayKey=RandomHex(32);
    try{SaveSettings();}catch(...){g.clients=std::move(old);g.relayKey=key;throw;}g.nonces.clear();g.lastClient.clear();g.lastSeen=0;CancelPower();ClosePairing();}
    if(rotate){StopRelay();StartRelay();}
}
void InitializeState(bool simulated,bool memoryOnly){
    InitializeCriticalSection(&g.lock);g.demo=simulated;g.memoryOnly=memoryOnly;g.id=RandomHex(16);wchar_t name[MAX_COMPUTERNAME_LENGTH+1];DWORD n=MAX_COMPUTERNAME_LENGTH+1;GetComputerNameW(name,&n);g.name=Utf8(name);
    SYSTEM_POWER_CAPABILITIES caps{};static_assert(sizeof(caps)==76,"Windows power capability ABI");
    // Older MinGW headers call bytes 18..20 spare2; Windows 10 names byte 20 AoAc.
    if(GetPwrCapabilities(&caps))g.sleep=caps.SystemS1||caps.SystemS2||caps.SystemS3||((const unsigned char*)&caps)[20];g.hibernate=IsPwrHibernateAllowed();if(simulated)g.sleep=g.hibernate=true;
    if(!memoryOnly){wchar_t appdata[MAX_PATH];if(FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,appdata)))throw Failure{500,"无法取得用户配置目录"};std::wstring folder=std::wstring(appdata)+L"\\PalmPower";CreateDirectoryW(folder.c_str(),nullptr);g.settingsPath=folder+L"\\settings.dat";ReadSettings();}
    CryptoInit();g.fingerprint=Fingerprint(g.modulus,g.exponent,g.id);g.session=RandomHex(16);if(simulated)RenewCode();else ClosePairing();
}
void CleanupState(){if(g.powerTimer){SetThreadpoolTimer(g.powerTimer,nullptr,0,0);WaitForThreadpoolTimerCallbacks(g.powerTimer,TRUE);CloseThreadpoolTimer(g.powerTimer);g.powerTimer=nullptr;}if(g.rsa)BCryptDestroyKey(g.rsa);if(g.rsaProvider)BCryptCloseAlgorithmProvider(g.rsaProvider,0);if(g.hmacProvider)BCryptCloseAlgorithmProvider(g.hmacProvider,0);DeleteCriticalSection(&g.lock);}
int RunSelfTests(const std::wstring&path){
    std::string report;auto check=[&](bool ok,const char*label){if(!ok)throw Failure{500,std::string("FAIL: ")+label};report+="PASS: "+std::string(label)+"\r\n";};
    auto denied=[](auto action){try{action();return false;}catch(...){return true;}};int code=0;
    try{
        check(g.demo,"All tests are wired to a mock power executor");
        std::string key=Base64(std::vector<unsigned char>(32,0));check(key=="AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=","Base64 matches reference encoding");
        check(Unbase64(key)==std::vector<unsigned char>(32,0),"Base64 decode round trip");
        check(Sign(key,"0123456789abcdef0123456789abcdef",1700000000,"00112233445566778899aabbccddeeff","shutdown",30)=="bybODxiv1AygyDt5g4ylNqIBdA+SBCjPuNRCxCTBYiQ=","Native HMAC matches Java, C# and Python reference");
        check(Json::parse("{\"name\":\"\\u4e2d\\u6587\\ud83d\\ude00\",\"n\":-12,\"b\":true}").at("name").str()=="中文😀","JSON Unicode and surrogate decoding");
        check(denied([]{Json::parse("{\"a\":1,\"a\":2}");}),"Duplicate JSON keys rejected");
        check(denied([]{Json::parse("{\"a\":01}");})&&denied([]{Json::parse("{\"a\":1.0}");}),"Ambiguous command numbers rejected");
        check(denied([]{Json::parse("{\"a\":\"\\ud800\"}");}),"Invalid Unicode rejected");
        check(denied([]{SchedulePower("format",30);}),"Unsupported power actions rejected");
        check(denied([]{SchedulePower("shutdown",0);})&&denied([]{SchedulePower("shutdown",3601);}),"Countdown bounds enforced");
        SchedulePower("shutdown",30);check(denied([]{SchedulePower("sleep",30);}),"Overlapping power actions rejected");CancelPower();check(g.action.empty(),"Cancellation clears pending action");
        PowerDue(nullptr,nullptr,nullptr);check(g.executed==0,"Cancelled action never executes");
        SchedulePower("sleep",10);PowerDue(nullptr,nullptr,nullptr);check(g.executed==0,"No early execution");{Guard lock(g.lock);g.deadline=GetTickCount64();}PowerDue(nullptr,nullptr,nullptr);PowerDue(nullptr,nullptr,nullptr);check(g.executed==1,"Mock power action executes only once");CancelPower();
        SchedulePower("hibernate",10);{Guard lock(g.lock);g.deadline=GetTickCount64();}PowerDue(nullptr,nullptr,nullptr);check(g.executed==2,"Mock hibernation follows the same countdown path");CancelPower();
        check(StartupCommand(L"C:\\App Folder\\GuanYuan.exe")==L"\"C:\\App Folder\\GuanYuan.exe\" --tray","Startup command quotes paths and starts silently");
        check(OnLink(htonl(INADDR_LOOPBACK)),"Loopback test transport allowed");
        std::string originalId=g.id;g.settingsPath=path+L".settings";g.memoryOnly=false;g.clients={{"0123456789abcdef0123456789abcdef","Test phone",key}};SaveSettings();g.clients.clear();ReadSettings();
        check(g.id==originalId&&g.clients.size()==1&&g.clients[0].secret==key,"Native DPAPI protected configuration persists");
        auto bytes=ReadFileBytes(g.settingsPath);check(std::string((char*)bytes.data(),bytes.size()).find(key)==std::string::npos,"Stored pairing key is not plaintext");DeleteFileW(g.settingsPath.c_str());g.memoryOnly=true;g.clients.clear();
        check(Unbase64(g.modulus).size()==256&&Unbase64(g.exponent).size()>=1,"RSA 2048 pairing public key exported");
        auto envelope=Seal(key,"0123456789abcdef0123456789abcdef","00112233445566778899aabbccddeeff","request","test payload");
        check(Open(key,envelope,"request")=="test payload","Native AES-256-GCM round trip");
        check(denied([&]{Open(key,envelope,"response");}),"Encryption binds message direction");
        Json changed=envelope;auto cipher=Unbase64(changed.at("data").str());cipher[0]^=1;changed["data"]=Base64(cipher);check(denied([&]{Open(key,changed,"request");}),"Modified ciphertext rejected");
        g.demo=false;bool httpDenied=denied([]{ValidateRelay("http://127.0.0.1:18779",std::string(64,'a'));});g.demo=true;check(httpDenied,"Production relay requires HTTPS even on loopback");
        check(denied([]{ValidateRelay("https://example.com?key=secret",std::string(64,'a'));})&&denied([]{ValidateRelay("https://user:pass@example.com",std::string(64,'a'));}),"Relay credentials in URL rejected");
        g.relayUrl="https://relay.example.com";g.relayKey=RandomHex(32);auto relayKey=g.relayKey;g.memoryOnly=false;SaveSettings();g.relayUrl.clear();g.relayKey.clear();ReadSettings();
        check(g.relayUrl=="https://relay.example.com"&&g.relayKey==relayKey,"Remote connection settings survive protected persistence");
        bytes=ReadFileBytes(g.settingsPath);check(std::string((char*)bytes.data(),bytes.size()).find(relayKey)==std::string::npos,"Stored relay access key is not plaintext");DeleteFileW(g.settingsPath.c_str());g.memoryOnly=true;g.relayUrl.clear();g.relayKey.clear();
        listening=true;const std::string phone="0123456789abcdef0123456789abcdef";g.clients={{phone,"Security fixture",key,1}};
        auto command=[&](const std::string&action,int delay){Json j=Json::obj();auto nonce=RandomHex(16);auto time=UnixNow();j["clientId"]=phone;j["nonce"]=nonce;j["timestamp"]=time;j["action"]=action;j["delay"]=delay;j["signature"]=Sign(key,phone,time,nonce,action,delay);return j;};
        check(denied([&]{Command(command("status",0).dump());}),"Legacy unverified pairing cannot control computer");g.clients[0].securityVersion=2;
        auto remote=[&](const Json&cmd,const std::string&session,int version){Json p=Json::obj();p["version"]=version;p["session"]=session;p["expires"]=UnixNow()+15;p["command"]=cmd;return Seal(key,phone,cmd.at("nonce").str(),"request",p.dump());};
        auto remoteCode=[&](const Json&envelope){return Json::parse(Open(key,RemoteCommand(envelope),"response")).at("status").integer();};
        check(Command(command("status",0).dump()).at("ok").boolean(),"Verified pairing accepted");
        check(remoteCode(remote(command("shutdown",30),g.session,1))==426,"Legacy remote payload refused");
        check(remoteCode(remote(command("shutdown",30),RandomHex(16),2))==409,"Wrong boot session refused");
        check(remoteCode(remote(command("shutdown",10),g.session,2))==400,"Internet power countdown below 30 seconds refused");
        auto captured=remote(command("shutdown",30),g.session,2);check(remoteCode(captured)==200,"Current session can schedule mock action");CancelPower();g.session=RandomHex(16);g.nonces.clear();
        check(remoteCode(captured)==409&&g.action.empty(),"Captured command refused after simulated reboot even with cleared nonce cache");
        listening=false;g.relayMode="ntfy";g.relayKey=RandomHex(32);auto oldTopic=NtfyTopic(g.relayKey);RenewCode();ClearClients();
        check(g.clients.empty()&&g.code.empty()&&NtfyTopic(g.relayKey)!=oldTopic,"Clearing pairing revokes clients, closes pairing and rotates ntfy route");g.relayKey.clear();
        report+="No actual power operation or Windows startup setting was changed.\r\n";
    }catch(const Failure&f){report+="FAIL: "+f.message+"\r\n";code=1;}catch(const std::exception&e){report+="FAIL: "+std::string(e.what())+"\r\n";code=1;}catch(...){report+="FAIL: unknown exception\r\n";code=1;}
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);if(file==INVALID_HANDLE_VALUE)return 1;DWORD written;WriteFile(file,report.data(),(DWORD)report.size(),&written,nullptr);CloseHandle(file);return code;
}
