#include "App.h"
#include <iphlpapi.h>
#include <algorithm>
#include <cctype>

std::atomic<bool> listening{false};
int boundPort=18677;
static SOCKET server=INVALID_SOCKET,discoverySocket=INVALID_SOCKET;
static HANDLE acceptThread=nullptr,discoveryThread=nullptr;
static SRWLOCK networkLock=SRWLOCK_INIT,workersLock=SRWLOCK_INIT;
static std::vector<SOCKET> workers;
static HANDLE workersDone=nullptr;
struct Address {uint32_t ip,mask;std::string text;};
static std::vector<Address> addresses;
static uint64_t addressesAt=0;
struct Exclusive {SRWLOCK*lock;Exclusive(SRWLOCK&l):lock(&l){AcquireSRWLockExclusive(lock);}~Exclusive(){ReleaseSRWLockExclusive(lock);}};
static void RefreshAddresses(){
    uint64_t now=GetTickCount64();if(addressesAt&&now-addressesAt<15000)return;
    ULONG n=16384;std::vector<unsigned char>data(n);ULONG result=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,(IP_ADAPTER_ADDRESSES*)data.data(),&n);
    if(result==ERROR_BUFFER_OVERFLOW){data.resize(n);result=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,nullptr,(IP_ADAPTER_ADDRESSES*)data.data(),&n);}
    std::vector<Address> fresh;
    if(result==NO_ERROR)for(auto adapter=(IP_ADAPTER_ADDRESSES*)data.data();adapter;adapter=adapter->Next){if(adapter->OperStatus!=IfOperStatusUp||adapter->IfType==IF_TYPE_SOFTWARE_LOOPBACK)continue;for(auto a=adapter->FirstUnicastAddress;a;a=a->Next){if(a->Address.lpSockaddr->sa_family!=AF_INET||a->OnLinkPrefixLength==0||a->OnLinkPrefixLength>32)continue;auto ip=(sockaddr_in*)a->Address.lpSockaddr;if((ntohl(ip->sin_addr.s_addr)>>16)==0xa9fe)continue;char text[INET_ADDRSTRLEN];InetNtopA(AF_INET,&ip->sin_addr,text,sizeof(text));fresh.push_back({ip->sin_addr.s_addr,htonl(0xffffffffu<<(32-a->OnLinkPrefixLength)),text});}}
    addresses=std::move(fresh);addressesAt=now;
}
bool OnLink(uint32_t remote){if((ntohl(remote)>>24)==127)return true;Exclusive lock(networkLock);RefreshAddresses();for(auto&a:addresses)if((a.ip&a.mask)==(remote&a.mask))return true;return false;}
std::string LanAddresses(){Exclusive lock(networkLock);RefreshAddresses();std::string text;for(auto&a:addresses){if(!text.empty())text+="  /  ";text+=a.text;}return text;}
static std::string Trim(std::string s){while(!s.empty()&&(s.back()==' '||s.back()=='\t'))s.pop_back();size_t start=s.find_first_not_of(" \t");return start==std::string::npos?"":s.substr(start);}
static std::string Lower(std::string s){for(char&c:s)c=(char)tolower((unsigned char)c);return s;}
struct Reader {
    SOCKET socket;std::string data;size_t position=0,total=0;uint64_t end=GetTickCount64()+5000;
    bool more(){if(GetTickCount64()>end)throw Failure{408,"请求超时"};char buffer[2048];int n=recv(socket,buffer,sizeof(buffer),0);if(n<=0)throw Failure{400,"连接已结束"};if(data.size()+(size_t)n>16384)throw Failure{413,"请求过大"};data.append(buffer,n);return true;}
    std::string line(){while(true){size_t e=data.find("\r\n",position);if(e!=std::string::npos){size_t len=e-position;if(total+len+2>8192)throw Failure{413,"请求头过大"};std::string value=data.substr(position,len);for(unsigned char c:value)if(c<32&&c!='\t')throw Failure{400,"请求头无效"};position=e+2;total+=len+2;return value;}if(data.size()-position+total>8192)throw Failure{413,"请求头过大"};more();}}
    std::string body(size_t n){while(data.size()-position<n)more();return data.substr(position,n);}
};
static bool SendAll(SOCKET socket,const std::string&data){size_t offset=0;while(offset<data.size()){int n=send(socket,data.data()+offset,(int)(data.size()-offset),0);if(n<=0)return false;offset+=(size_t)n;}return true;}
static void Respond(SOCKET socket,int code,const Json&json){std::string body=json.dump();std::string header="HTTP/1.1 "+std::to_string(code)+" Result\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: "+std::to_string(body.size())+"\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n";SendAll(socket,header+body);}
static void Serve(SOCKET socket){
    DWORD timeout=2000;setsockopt(socket,SOL_SOCKET,SO_RCVTIMEO,(char*)&timeout,sizeof(timeout));setsockopt(socket,SOL_SOCKET,SO_SNDTIMEO,(char*)&timeout,sizeof(timeout));
    int code=200;Json result;
    try{
        Reader reader{socket};std::string request=reader.line();size_t first=request.find(' '),last=request.rfind(' ');if(first==std::string::npos||first==last||request.find(' ',first+1)!=last)throw Failure{400,"请求无效"};
        std::string method=request.substr(0,first),path=request.substr(first+1,last-first-1),version=request.substr(last+1);if(version!="HTTP/1.1"&&version!="HTTP/1.0")throw Failure{400,"请求无效"};
        std::map<std::string,std::string>headers;while(true){std::string line=reader.line();if(line.empty())break;size_t colon=line.find(':');if(colon==std::string::npos||colon==0)throw Failure{400,"请求头无效"};std::string key=Lower(Trim(line.substr(0,colon)));if(headers.count(key))throw Failure{400,"重复请求头"};headers[key]=Trim(line.substr(colon+1));}
        if(headers.count("transfer-encoding"))throw Failure{400,"不支持分块请求"};size_t length=0;
        if(headers.count("content-length")){const auto&s=headers["content-length"];if(s.empty()||s.size()>5)throw Failure{413,"请求过大"};for(char c:s){if(c<'0'||c>'9')throw Failure{400,"请求长度无效"};length=length*10+c-'0';}if(length>4096)throw Failure{413,"请求过大"};}
        std::string body;if(method=="POST"){if(!headers.count("content-type")||Lower(headers["content-type"]).find("application/json")!=0)throw Failure{415,"需要 JSON 请求"};body=reader.body(length);Wide(body);}
        sockaddr_in remote{};int size=sizeof(remote);getpeername(socket,(sockaddr*)&remote,&size);char ip[INET_ADDRSTRLEN];InetNtopA(AF_INET,&remote.sin_addr,ip,sizeof(ip));
        if(method=="GET"&&path=="/info")result=Info();else if(method=="POST"&&path=="/pair")result=Pair(body,ip);else if(method=="POST"&&path=="/command")result=Command(body);else throw Failure{404,"接口不存在"};
    }catch(const Failure&error){code=error.code;result=Json::obj();result["ok"]=false;result["error"]=error.message;}
    catch(const std::exception&){code=400;result=Json::obj();result["ok"]=false;result["error"]="请求内容无效";}
    catch(...){code=500;result=Json::obj();result["ok"]=false;result["error"]="电脑暂时无法处理请求";}
    Respond(socket,code,result);
}
static DWORD WINAPI Worker(void*arg){SOCKET socket=(SOCKET)(uintptr_t)arg;try{Serve(socket);}catch(...){}
    {Exclusive lock(workersLock);auto p=std::find(workers.begin(),workers.end(),socket);if(p!=workers.end())workers.erase(p);shutdown(socket,SD_BOTH);closesocket(socket);if(workers.empty())SetEvent(workersDone);}return 0;
}
static DWORD WINAPI AcceptLoop(void*){
    while(listening){sockaddr_in remote{};int n=sizeof(remote);SOCKET socket=accept(server,(sockaddr*)&remote,&n);if(socket==INVALID_SOCKET){if(!listening)break;Sleep(10);continue;}if(!OnLink(remote.sin_addr.s_addr)){closesocket(socket);continue;}
        Exclusive lock(workersLock);if(!listening||workers.size()>=8){closesocket(socket);continue;}workers.push_back(socket);ResetEvent(workersDone);HANDLE thread=CreateThread(nullptr,128*1024,Worker,(void*)(uintptr_t)socket,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
        if(thread)CloseHandle(thread);else{workers.pop_back();closesocket(socket);if(workers.empty())SetEvent(workersDone);}
    }return 0;
}
static DWORD WINAPI DiscoveryLoop(void*){
    while(listening){char buffer[128];sockaddr_in remote{};int n=sizeof(remote);int got=recvfrom(discoverySocket,buffer,sizeof(buffer),0,(sockaddr*)&remote,&n);if(got==SOCKET_ERROR){if(!listening)break;Sleep(10);continue;}
        if(got!=23||memcmp(buffer,"PALM_POWER_DISCOVER_V1!",23)||!OnLink(remote.sin_addr.s_addr))continue;
        Json j=Json::obj();j["app"]="PalmPower";j["version"]=1;{Guard guard(g.lock);j["id"]=g.id;j["name"]=g.name;}j["port"]=boundPort;std::string text=j.dump();sendto(discoverySocket,text.data(),(int)text.size(),0,(sockaddr*)&remote,n);
    }return 0;
}
bool StartService(bool loopback,int port,bool discovery){
    if(listening)return true;server=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(server==INVALID_SOCKET)return false;
    BOOL exclusive=TRUE;setsockopt(server,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(char*)&exclusive,sizeof(exclusive));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(loopback?INADDR_LOOPBACK:INADDR_ANY);address.sin_port=htons((u_short)port);
    if(bind(server,(sockaddr*)&address,sizeof(address))||listen(server,8)){closesocket(server);server=INVALID_SOCKET;return false;}
    int n=sizeof(address);getsockname(server,(sockaddr*)&address,&n);boundPort=ntohs(address.sin_port);
    if(discovery){discoverySocket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);address.sin_port=htons(18678);setsockopt(discoverySocket,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(char*)&exclusive,sizeof(exclusive));if(discoverySocket==INVALID_SOCKET||bind(discoverySocket,(sockaddr*)&address,sizeof(address))){closesocket(server);if(discoverySocket!=INVALID_SOCKET)closesocket(discoverySocket);server=discoverySocket=INVALID_SOCKET;return false;}}
    if(!workersDone)workersDone=CreateEventW(nullptr,TRUE,TRUE,nullptr);listening=true;acceptThread=CreateThread(nullptr,128*1024,AcceptLoop,nullptr,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);if(discovery)discoveryThread=CreateThread(nullptr,128*1024,DiscoveryLoop,nullptr,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
    if(!acceptThread||(discovery&&!discoveryThread)){StopService();return false;}StartRelay();NotifyUi();return true;
}
void StopService(){
    listening=false;StopRelay();if(server!=INVALID_SOCKET){shutdown(server,SD_BOTH);closesocket(server);server=INVALID_SOCKET;}if(discoverySocket!=INVALID_SOCKET){closesocket(discoverySocket);discoverySocket=INVALID_SOCKET;}
    if(acceptThread){WaitForSingleObject(acceptThread,3000);CloseHandle(acceptThread);acceptThread=nullptr;}if(discoveryThread){WaitForSingleObject(discoveryThread,3000);CloseHandle(discoveryThread);discoveryThread=nullptr;}
    {Exclusive lock(workersLock);for(auto socket:workers)shutdown(socket,SD_BOTH);}if(workersDone)WaitForSingleObject(workersDone,3000);
    try{CancelPower();}catch(...){}NotifyUi();
}
