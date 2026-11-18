#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <map>
#include <atomic>
#include "Json.h"

struct Failure {int code;std::string message;};
struct Guard {CRITICAL_SECTION* p;Guard(CRITICAL_SECTION& cs):p(&cs){EnterCriticalSection(p);}~Guard(){LeaveCriticalSection(p);}};
struct Client {std::string id,name,secret;int securityVersion=1;};
struct State {
    CRITICAL_SECTION lock;
    std::string id,name,code,action,message="等待手机连接",lastClient;
    uint64_t codeExpires=0,deadline=0,lastSeen=0;
    bool demo=false,executing=false,sleep=false,hibernate=false,memoryOnly=false;
    int executed=0;
    std::vector<Client> clients;
    std::map<std::string,uint64_t> nonces;
    std::map<std::string,std::vector<uint64_t>> attempts;
    std::vector<uint64_t> pairAttempts;
    BCRYPT_ALG_HANDLE rsaProvider=nullptr,hmacProvider=nullptr;
    BCRYPT_KEY_HANDLE rsa=nullptr;
    std::string modulus,exponent;
    std::string fingerprint,session;
    PTP_TIMER powerTimer=nullptr;
    std::wstring settingsPath;
    std::string relayUrl,relayKey,relayMode="private",relayMessage="跨网络：未设置";
};
extern State g;
extern HWND mainWindow;
extern HINSTANCE instance;
extern std::atomic<bool> listening;
extern int boundPort;
constexpr UINT WM_UPDATE=WM_APP+10,WM_TRAY=WM_APP+11;
std::wstring Wide(const std::string& s);
std::string Utf8(const std::wstring& s);
int64_t UnixNow();
std::string Base64(const std::vector<unsigned char>& data);
std::vector<unsigned char> Unbase64(const std::string& value);
std::vector<unsigned char> RandomBytes(size_t count);
std::string RandomHex(size_t count);
std::string Sign(const std::string& secret,const std::string& id,int64_t time,const std::string& nonce,const std::string& action,int delay);
bool Equal(const std::string& a,const std::string& b);
void InitializeState(bool simulated,bool memoryOnly);
void CleanupState();
void SaveSettings();
void RenewCode();
void ClosePairing();
std::string Fingerprint(const std::string& modulus,const std::string& exponent,const std::string& id);
std::wstring FingerprintLabel();
void CancelPower();
void SchedulePower(const std::string& action,int delay);
void ClearClients();
Json Info();
Json PowerStatus();
Json Pair(const std::string& body,const std::string& ip);
Json Command(const std::string& body);
void NotifyUi();
bool StartService(bool loopback=false,int port=18677,bool discovery=true);
void StopService();
bool OnLink(uint32_t remote);
std::string LanAddresses();
std::wstring ExecutablePath();
std::wstring StartupCommand(const std::wstring& path);
bool StartupEnabled();
bool SetStartup(bool enabled);
void ShowPanel();
int RunSelfTests(const std::wstring& path);
Json Seal(const std::string& secret,const std::string& client,const std::string& request,const std::string& direction,const std::string& plain);
std::string Open(const std::string& secret,const Json& envelope,const std::string& direction);
Json RemoteCommand(const Json& envelope);
void ValidateRelay(const std::string& url,const std::string& key);
void ConfigureRelay(const std::string& url,const std::string& key,const std::string& mode="private");
std::string NtfyTopic(const std::string& key,const std::string& client="");
void StartRelay();
void StopRelay();
