#include "App.h"
#include <shellapi.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>

HWND mainWindow=nullptr;
HINSTANCE instance=nullptr;
static HICON appIcon=nullptr,heroIcon=nullptr;
static HFONT regular=nullptr,small=nullptr,bold=nullptr,titleFont=nullptr,codeFont=nullptr,captionFont=nullptr;
static HBRUSH bgBrush=nullptr;
static bool built=false,exiting=false,preview=false;
static int dpi=96;
static UINT taskbarCreated=0;
static NOTIFYICONDATAW tray{};
static std::string panelSnapshot;
static bool uiTesting=false;
static unsigned panelInvalidations=0;
static WNDPROC nativeButtonProc=nullptr;
static HDC panelBuffer=nullptr;
static HBITMAP panelBitmap=nullptr;
static HGDIOBJ originalBitmap=nullptr;
static SIZE bufferSize{};
static bool panelBufferDirty=true;
static constexpr COLORREF BG=RGB(250,246,245),CARD=RGB(255,255,255),INK=RGB(45,35,38),MUTED=RGB(105,87,92),RED=RGB(166,37,57),SOFT=RGB(247,230,233);
static constexpr COLORREF TITLE_BG=RGB(248,232,235),TITLE_INK=RGB(75,47,55),WINDOW_EDGE=RGB(196,147,158);
static constexpr int PanelWidth=760,PanelHeight=680,TitleHeight=46;
static constexpr wchar_t WindowClass[]=L"PalmPower.NativeWindow";
static constexpr UINT ShowExisting=WM_APP+12,QueryBuild=WM_APP+13;
static constexpr LRESULT DesktopBuild=0x010309;
static constexpr int TOGGLE=1001,CANCEL=1002,CLEAR=1003,EXIT=1004,RENEW=1005,FIREWALL=1006,STARTUP=1007,OPEN=1008,REMOTE=1009;
static constexpr int CAPTION_MIN=1010,CAPTION_CLOSE=1011;
static constexpr int CaptionStart=648;
struct ButtonLayout {
    int id;
    const wchar_t* label;
    int x,y,width,height;
};
static constexpr ButtonLayout PanelButtons[]={
    {RENEW,L"开始配对",48,309,152,44},
    {FIREWALL,L"允许局域网连接",406,309,184,44},
    {TOGGLE,L"停止服务",28,526,136,44},
    {CANCEL,L"取消倒计时",178,526,136,44},
    {CLEAR,L"清除配对",328,526,108,44},
    {REMOTE,L"跨网络设置",450,526,140,44},
    {EXIT,L"退出程序",604,526,128,44},
    {CAPTION_MIN,L"最小化到托盘",CaptionStart,4,44,38},
    {CAPTION_CLOSE,L"关闭窗口到托盘",702,4,44,38},
};
static int S(int n){return MulDiv(n,dpi,96);}
static RECT R(int x,int y,int w,int h){return RECT{S(x),S(y),S(x+w),S(y+h)};}
static std::wstring ActionLabel(const std::string&a){return a=="shutdown"?L"关机":a=="sleep"?L"睡眠":L"休眠";}
std::wstring ExecutablePath(){std::wstring path(32768,0);DWORD n=GetModuleFileNameW(nullptr,&path[0],(DWORD)path.size());path.resize(n);return path;}
std::wstring StartupCommand(const std::wstring&path){return L"\""+path+L"\" --tray";}
bool StartupEnabled(){HKEY key=nullptr;if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS)return false;DWORD type=0,size=0;LONG result=RegQueryValueExW(key,L"PalmPower",nullptr,&type,nullptr,&size);std::wstring value(size/sizeof(wchar_t)+1,0);if(result==ERROR_SUCCESS&&type==REG_SZ&&size<65536)result=RegQueryValueExW(key,L"PalmPower",nullptr,&type,(BYTE*)&value[0],&size);else result=ERROR_INVALID_DATA;RegCloseKey(key);value.resize(wcslen(value.c_str()));return result==ERROR_SUCCESS&&value==StartupCommand(ExecutablePath());}
bool SetStartup(bool enabled){
    if(g.demo){MessageBoxW(mainWindow,L"模拟模式不修改开机自启设置。请退出模拟模式后正常启动。",L"关源",MB_OK|MB_ICONINFORMATION);return false;}
    HKEY key=nullptr;if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS)return false;
    LONG result;if(enabled){std::wstring command=StartupCommand(ExecutablePath());result=RegSetValueExW(key,L"PalmPower",0,REG_SZ,(BYTE*)command.c_str(),(DWORD)((command.size()+1)*sizeof(wchar_t)));}else{result=RegDeleteValueW(key,L"PalmPower");if(result==ERROR_FILE_NOT_FOUND)result=ERROR_SUCCESS;}RegCloseKey(key);return result==ERROR_SUCCESS;
}
void NotifyUi(){if(mainWindow&&IsWindowVisible(mainWindow))PostMessageW(mainWindow,WM_UPDATE,0,0);}
static void Text(HDC dc,const std::wstring&value,RECT rect,HFONT font,COLORREF color,UINT flags=DT_LEFT|DT_TOP|DT_NOPREFIX){SelectObject(dc,font);SetTextColor(dc,color);SetBkMode(dc,TRANSPARENT);DrawTextW(dc,value.c_str(),(int)value.size(),&rect,flags);}
static void Fill(HDC dc,const RECT& rect,COLORREF color){
    const COLORREF old=SetDCBrushColor(dc,color);
    FillRect(dc,&rect,reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetDCBrushColor(dc,old);
}

static constexpr int SamplesPerAxis=8;
static constexpr int TotalSamples=SamplesPerAxis*SamplesPerAxis;

static const std::vector<uint8_t>& CornerCoverage(int radius){
    static std::map<int,std::vector<uint8_t>> masks;
    auto [entry,inserted]=masks.try_emplace(radius);
    auto& mask=entry->second;
    if(!inserted)return mask;
    mask.resize(size_t(radius)*radius);
    const int center=radius*SamplesPerAxis*2;
    for(int y=0;y<radius;y++){
        for(int x=0;x<radius;x++){
            int covered=0;
            for(int sy=0;sy<SamplesPerAxis;sy++){
                const int dy=center-1-2*(y*SamplesPerAxis+sy);
                for(int sx=0;sx<SamplesPerAxis;sx++){
                    const int dx=center-1-2*(x*SamplesPerAxis+sx);
                    if(dx*dx+dy*dy<=center*center)++covered;
                }
            }
            mask[y*radius+x]=static_cast<uint8_t>(covered);
        }
    }
    return mask;
}

static void Box(HDC dc,const RECT& rect,COLORREF color,int radius=24,COLORREF surface=BG){
    const int width=rect.right-rect.left,height=rect.bottom-rect.top;
    const int r=std::min({S(radius),width/2,height/2});
    if(r<=0){Fill(dc,rect,color);return;}
    Fill(dc,{rect.left+r,rect.top,rect.right-r,rect.bottom},color);
    Fill(dc,{rect.left,rect.top+r,rect.left+r,rect.bottom-r},color);
    Fill(dc,{rect.right-r,rect.top+r,rect.right,rect.bottom-r},color);

    // Composite subpixel coverage over the control's own surface. Only corner
    // pixels need blending. The solid center is painted directly above.
    const auto& coverage=CornerCoverage(r);
    const size_t area=coverage.size();
    std::vector<uint32_t> pixels(area*4);
    for(int y=0;y<r;y++){
        for(int x=0;x<r;x++){
            const int covered=coverage[y*r+x];
            auto blend=[covered](int fg,int bg){return (fg*covered+bg*(TotalSamples-covered)+TotalSamples/2)/TotalSamples;};
            const uint32_t pixel=(blend(GetRValue(color),GetRValue(surface))<<16)
                |(blend(GetGValue(color),GetGValue(surface))<<8)|blend(GetBValue(color),GetBValue(surface));
            pixels[y*r+x]=pixel;
            pixels[area+y*r+(r-1-x)]=pixel;
            pixels[2*area+(r-1-y)*r+x]=pixel;
            pixels[3*area+(r-1-y)*r+(r-1-x)]=pixel;
        }
    }
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth=r;
    bitmap.bmiHeader.biHeight=-r;
    bitmap.bmiHeader.biPlanes=1;
    bitmap.bmiHeader.biBitCount=32;
    bitmap.bmiHeader.biCompression=BI_RGB;
    const LONG xs[]={rect.left,rect.right-r,rect.left,rect.right-r};
    const LONG ys[]={rect.top,rect.top,rect.bottom-r,rect.bottom-r};
    for(int corner=0;corner<4;corner++){
        StretchDIBits(dc,xs[corner],ys[corner],r,r,0,0,r,r,pixels.data()+corner*area,&bitmap,DIB_RGB_COLORS,SRCCOPY);
    }
}

static void ButtonFace(HDC dc,RECT rect,int radius,COLORREF color,COLORREF surface,bool keyboardFocus){
    if(keyboardFocus){
        Box(dc,rect,RED,radius,surface);
        const int inset=std::max(2,S(2));
        InflateRect(&rect,-inset,-inset);
        Box(dc,rect,surface,radius-2,RED);
        InflateRect(&rect,-inset,-inset);
        radius-=4;
    }
    Box(dc,rect,color,radius,surface);
}

static void DrawButton(HDC dc,RECT rect,int id,const wchar_t* label,UINT state,bool hovered=false){
    const bool caption=id==CAPTION_MIN||id==CAPTION_CLOSE;
    const bool primary=id==TOGGLE||id==703||id==IDOK;
    const bool enabled=!(state&ODS_DISABLED);
    const bool pressed=(state&ODS_SELECTED)!=0;
    const bool keyboardFocus=enabled&&(state&ODS_FOCUS)&&!(state&ODS_NOFOCUSRECT);
    const COLORREF surface=caption?TITLE_BG:(id==RENEW||id==FIREWALL?CARD:BG);
    Fill(dc,rect,surface);
    if(caption){
        if(hovered||pressed||keyboardFocus){
            RECT highlight=rect;
            InflateRect(&highlight,-S(2),-S(2));
            ButtonFace(dc,highlight,15,id==CAPTION_CLOSE?RGB(242,198,206):RGB(238,212,220),surface,keyboardFocus);
        }
        Text(dc,id==CAPTION_MIN?L"−":L"×",rect,captionFont,TITLE_INK,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    }else{
        COLORREF button=SOFT;
        if(!enabled)button=RGB(241,236,235);
        else if(primary)button=pressed?RGB(128,24,38):hovered?RGB(151,30,47):RED;
        else if(pressed||hovered)button=pressed?RGB(236,197,201):RGB(244,214,218);
        ButtonFace(dc,rect,22,button,surface,keyboardFocus);
        COLORREF textColor=!enabled?RGB(119,105,112):primary?CARD:RED;
        Text(dc,label,rect,bold,textColor,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    }
}

static LRESULT CALLBACK ButtonHover(HWND hwnd,UINT message,WPARAM w,LPARAM l){
    if(message==WM_LBUTTONDOWN){
        SendMessageW(GetParent(hwnd),WM_CHANGEUISTATE,MAKEWPARAM(UIS_SET,UISF_HIDEFOCUS),0);
    }
    if(message==WM_MOUSEMOVE&&!GetWindowLongPtrW(hwnd,GWLP_USERDATA)&&IsWindowEnabled(hwnd)){
        TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,hwnd,0};
        if(TrackMouseEvent(&tracking)){
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,1);
            InvalidateRect(hwnd,nullptr,FALSE);
        }
    }else if(message==WM_MOUSELEAVE||message==WM_ENABLE){
        SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);
        InvalidateRect(hwnd,nullptr,FALSE);
    }else if(message==WM_UPDATEUISTATE){
        InvalidateRect(hwnd,nullptr,FALSE);
    }
    return CallWindowProcW(nativeButtonProc,hwnd,message,w,l);
}

static HWND Button(int id,const wchar_t*label,int x,int y,int width,int height=40,HWND parent=nullptr){
    if(!parent)parent=mainWindow;
    HWND button=CreateWindowExW(0,L"BUTTON",label,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
        S(x),S(y),S(width),S(height),parent,(HMENU)(INT_PTR)id,instance,nullptr);
    SendMessageW(button,WM_SETFONT,(WPARAM)bold,FALSE);
    // All instances use the same standard Unicode BUTTON class on this UI thread.
    WNDPROC previous=reinterpret_cast<WNDPROC>(GetWindowLongPtrW(button,GWLP_WNDPROC));
    if(!nativeButtonProc)nativeButtonProc=previous;
    if(previous&&previous==nativeButtonProc)SetWindowLongPtrW(button,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(ButtonHover));
    return button;
}

static void DrawButtonItem(const DRAWITEMSTRUCT& item){
    wchar_t label[128]{};
    GetWindowTextW(item.hwndItem,label,128);
    const bool hover=GetWindowLongPtrW(item.hwndItem,GWLP_USERDATA)!=0;
    UINT state=item.itemState;
    if(SendMessageW(item.hwndItem,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)state|=ODS_NOFOCUSRECT;
    DrawButton(item.hDC,item.rcItem,item.CtlID,label,state,hover);
}

static bool RoundWindow(HWND hwnd){
    const DWM_WINDOW_CORNER_PREFERENCE corners=DWMWCP_ROUND;
    return SUCCEEDED(DwmSetWindowAttribute(hwnd,DWMWA_WINDOW_CORNER_PREFERENCE,&corners,sizeof(corners)));
}
static void UpdateButtons(){
    if(!built)return;Json state=PowerStatus();wchar_t current[32];GetDlgItemTextW(mainWindow,TOGGLE,current,32);const wchar_t* label=listening?L"停止服务":L"启动服务";if(wcscmp(current,label))SetWindowTextW(GetDlgItem(mainWindow,TOGGLE),label);
    HWND cancel=GetDlgItem(mainWindow,CANCEL);bool enabled=!state.at("action").str().empty();if((IsWindowEnabled(cancel)!=FALSE)!=enabled)EnableWindow(cancel,enabled);
    HWND check=GetDlgItem(mainWindow,STARTUP);LRESULT value=!g.demo&&StartupEnabled()?BST_CHECKED:BST_UNCHECKED;if(SendMessageW(check,BM_GETCHECK,0,0)!=value)SendMessageW(check,BM_SETCHECK,value,0);
    GetDlgItemTextW(mainWindow,RENEW,current,32);{Guard guard(g.lock);label=GetTickCount64()<g.codeExpires?L"关闭配对":L"开始配对";}if(wcscmp(current,label))SetWindowTextW(GetDlgItem(mainWindow,RENEW),label);
}
static void BuildPanel(){
    if(built)return;
    built=true;
    panelSnapshot.clear();
    for(const auto& button:PanelButtons)Button(button.id,button.label,button.x,button.y,button.width,button.height);
    HWND check=CreateWindowExW(0,L"BUTTON",L"开机自启 · 登录 Windows 后静默驻留托盘",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,S(30),S(585),S(650),S(28),mainWindow,(HMENU)(INT_PTR)STARTUP,instance,nullptr);SendMessageW(check,WM_SETFONT,(WPARAM)regular,FALSE);EnableWindow(check,!g.demo);
    heroIcon=(HICON)LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,S(56),S(56),0);UpdateButtons();
}
static void PaintPanel(HDC dc){
    RECT entire=R(0,0,PanelWidth,PanelHeight);FillRect(dc,&entire,bgBrush);
    Fill(dc,R(0,0,PanelWidth,TitleHeight),TITLE_BG);
    Fill(dc,R(0,TitleHeight,PanelWidth,1),RGB(231,207,214));
    const RECT edges[]={R(0,0,PanelWidth,1),R(0,0,1,PanelHeight),R(PanelWidth-1,0,1,PanelHeight),R(0,PanelHeight-1,PanelWidth,1)};
    for(const RECT& edge:edges)Fill(dc,edge,WINDOW_EDGE);
    Text(dc,L"关源",R(22,0,96,TitleHeight),bold,TITLE_INK,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    Text(dc,L"v1.3.9",R(64,0,130,TitleHeight),small,MUTED,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    if(heroIcon)DrawIconEx(dc,S(28),S(75),heroIcon,S(56),S(56),0,nullptr,DI_NORMAL);
    Text(dc,L"关源",R(99,72,360,40),titleFont,INK);
    Text(dc,L"用手机控制这台电脑",R(101,111,390,22),small,MUTED);
    Box(dc,R(565,89,167,30),SOFT,15);
    Text(dc,g.demo?L"●  模拟模式":listening?L"●  局域网已开启":L"●  服务已停止",R(565,89,167,30),bold,RED,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    Box(dc,R(28,158,340,205),CARD);Box(dc,R(386,158,346,205),CARD);Box(dc,R(28,382,704,123),CARD);
    Text(dc,L"首次配对",R(48,177,300,26),bold,INK);Text(dc,L"核对手机指纹，再输入配对码",R(48,207,300,24),small,MUTED);
    Text(dc,FingerprintLabel(),R(48,232,302,21),small,MUTED);
    std::wstring pin;{Guard guard(g.lock);pin=GetTickCount64()<g.codeExpires?Wide(g.code):L"未开启";}if(pin.size()==6)pin.insert(3,L" ");Text(dc,pin,R(46,252,302,53),codeFont,RED);
    const bool previewFrame=preview&&!uiTesting;
    Text(dc,L"自动发现",R(406,177,304,26),bold,INK);
    Text(dc,previewFrame?L"我的电脑":Wide(g.name),R(406,211,300,30),bold,INK,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
    std::string ips=previewFrame?"192.0.2.10":LanAddresses();
    Text(dc,ips.empty()?L"请连接 Wi-Fi 或有线局域网":L"本机地址  "+Wide(ips),R(406,253,305,49),small,MUTED,DT_LEFT|DT_TOP|DT_WORDBREAK|DT_NOPREFIX);
    Text(dc,L"当前状态",R(48,399,500,22),small,MUTED);
    Json state=PowerStatus();std::string action=state.at("action").str();Text(dc,action.empty()?Wide(state.at("message").str()):L"即将"+ActionLabel(action)+(g.demo?L"（模拟）":L""),R(48,430,526,32),bold,INK);
    std::wstring count=action.empty()?L"—":std::to_wstring(state.at("remaining").integer())+L"s";Text(dc,count,R(594,418,116,58),codeFont,RED,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
    std::wstring clients;{Guard guard(g.lock);clients=L"已配对 "+std::to_wstring(g.clients.size())+L" 台手机";if(g.lastSeen&&GetTickCount64()-g.lastSeen<12000)clients+=L"   ·   "+Wide(g.lastClient)+L" 已连接";}
    Text(dc,clients,R(48,471,600,23),small,MUTED);
    std::string remote;{Guard guard(g.lock);remote=g.relayMessage;}Text(dc,Wide(remote),R(30,628,700,22),small,MUTED);
    Text(dc,L"关闭窗口后在托盘待机；退出程序会取消待执行操作。",R(30,651,700,22),small,MUTED);
}
static void RefreshPanel(){
    if(!built||(!uiTesting&&!IsWindowVisible(mainWindow)))return;
    Json visible=PowerStatus();visible["listening"]=listening.load();visible["addresses"]=LanAddresses();visible["startup"]=!g.demo&&StartupEnabled();
    {Guard guard(g.lock);visible["pin"]=GetTickCount64()<g.codeExpires?g.code:"expired";visible["paired"]=(int)g.clients.size();visible["client"]=g.lastSeen&&GetTickCount64()-g.lastSeen<12000?g.lastClient:"";}
    std::string next=visible.dump();if(next==panelSnapshot)return;panelSnapshot=std::move(next);UpdateButtons();panelBufferDirty=true;panelInvalidations++;InvalidateRect(mainWindow,nullptr,FALSE);
}
static void ReleasePanelBuffer(){
    if(panelBuffer){
        if(originalBitmap)SelectObject(panelBuffer,originalBitmap);
        DeleteDC(panelBuffer);
    }
    if(panelBitmap)DeleteObject(panelBitmap);
    panelBuffer=nullptr;
    panelBitmap=nullptr;
    originalBitmap=nullptr;
    bufferSize={};
    panelBufferDirty=true;
}

static void PaintBuffered(HWND hwnd,HDC target){
    RECT rect{};
    GetClientRect(hwnd,&rect);
    const LONG width=rect.right,height=rect.bottom;
    if(width<=0||height<=0)return;
    if(!panelBuffer||bufferSize.cx!=width||bufferSize.cy!=height){
        ReleasePanelBuffer();
        panelBuffer=CreateCompatibleDC(target);
        panelBitmap=panelBuffer?CreateCompatibleBitmap(target,width,height):nullptr;
        if(!panelBitmap){ReleasePanelBuffer();PaintPanel(target);return;}
        originalBitmap=SelectObject(panelBuffer,panelBitmap);
        bufferSize={width,height};
    }
    if(panelBufferDirty){
        PaintPanel(panelBuffer);
        panelBufferDirty=false;
    }
    BitBlt(target,0,0,width,height,panelBuffer,0,0,SRCCOPY);
}
void ShowPanel(){BuildPanel();ShowWindow(mainWindow,SW_RESTORE);SetForegroundWindow(mainWindow);SetTimer(mainWindow,1,1000,nullptr);RefreshPanel();}
static void HidePanel(){
    ShowWindow(mainWindow,SW_HIDE);
    KillTimer(mainWindow,1);
    ReleasePanelBuffer();
    if(!built)return;
    built=false;
    for(const auto& button:PanelButtons)DestroyWindow(GetDlgItem(mainWindow,button.id));
    DestroyWindow(GetDlgItem(mainWindow,STARTUP));
    if(heroIcon){DestroyIcon(heroIcon);heroIcon=nullptr;}
}
static void ExitApp(){Json state=PowerStatus();if(!state.at("action").str().empty()&&MessageBoxW(mainWindow,L"退出将取消待执行操作，确定退出？",L"退出",MB_YESNO|MB_ICONQUESTION)!=IDYES)return;exiting=true;DestroyWindow(mainWindow);}
static void Firewall(){std::wstring exe=ExecutablePath();size_t at=exe.find_last_of(L"\\/");std::wstring script=exe.substr(0,at+1)+L"AllowNetwork.ps1";if(GetFileAttributesW(script.c_str())==INVALID_FILE_ATTRIBUTES){MessageBoxW(mainWindow,L"请完整解压电脑端文件夹，确保 AllowNetwork.ps1 与程序在一起。",L"缺少文件",MB_OK|MB_ICONINFORMATION);return;}
    std::wstring arguments=L"-NoProfile -ExecutionPolicy Bypass -File \""+script+L"\"";wchar_t system[MAX_PATH];GetSystemDirectoryW(system,MAX_PATH);std::wstring powershell=std::wstring(system)+L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    SHELLEXECUTEINFOW info{sizeof(info)};info.hwnd=mainWindow;info.lpVerb=L"runas";info.lpFile=powershell.c_str();info.lpParameters=arguments.c_str();info.nShow=SW_HIDE;if(!ShellExecuteExW(&info)&&GetLastError()!=ERROR_CANCELLED)MessageBoxW(mainWindow,L"无法打开网络设置脚本。",L"提示",MB_OK|MB_ICONINFORMATION);
}
static INT_PTR CALLBACK RemoteDialog(HWND hwnd,UINT msg,WPARAM w,LPARAM l){
    if(msg==WM_INITDIALOG){
        SetWindowTextW(hwnd,L"关源 · 跨网络设置");RECT parent;GetWindowRect(mainWindow,&parent);SetWindowPos(hwnd,nullptr,parent.left+(parent.right-parent.left-S(558))/2,parent.top+(parent.bottom-parent.top-S(414))/2,S(558),S(414),SWP_NOZORDER);
        RoundWindow(hwnd);
        auto control=[&](const wchar_t* cls,const wchar_t* text,DWORD style,int id,int x,int y,int width,int height){HWND c=CreateWindowExW(wcscmp(cls,L"EDIT")==0?WS_EX_CLIENTEDGE:0,cls,text,WS_CHILD|WS_VISIBLE|style,S(x),S(y),S(width),S(height),hwnd,(HMENU)(INT_PTR)id,instance,nullptr);SendMessageW(c,WM_SETFONT,(WPARAM)regular,TRUE);return c;};
        control(L"STATIC",L"开启后，让手机在同一 Wi-Fi 连接一次，即可保存远程入口。\n免服务器模式使用 ntfy 公共服务，只传递加密消息。",0,0,16,14,520,44);
        Button(703,L"一键开启免服务器模式",16,70,510,38,hwnd);
        SendMessageW(hwnd,DM_SETDEFID,703,0);
        control(L"STATIC",L"无需账号、服务器或域名；连接受公共服务可用性和配额限制。",0,0,16,116,510,22);
        control(L"STATIC",L"自有中转（可选） · 地址 HTTPS",0,0,16,156,510,22);HWND url=control(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,701,16,183,510,28);SendMessageW(url,EM_SETLIMITTEXT,512,0);
        control(L"STATIC",L"自有中转访问密钥",0,0,16,225,510,22);HWND key=control(L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL|ES_PASSWORD,702,16,252,510,28);SendMessageW(key,EM_SETLIMITTEXT,64,0);
        {Guard guard(g.lock);if(g.relayMode=="private"){SetWindowTextW(url,Wide(g.relayUrl).c_str());SetWindowTextW(key,Wide(g.relayKey).c_str());}else SetWindowTextW(GetDlgItem(hwnd,703),L"免服务器模式已开启 · 点击重新连接");}
        Button(704,L"关闭跨网络",16,327,122,34,hwnd);Button(IDOK,L"保存自有中转",268,327,137,34,hwnd);Button(IDCANCEL,L"返回",421,327,105,34,hwnd);return TRUE;
    }
    if(msg==WM_DRAWITEM){DrawButtonItem(*reinterpret_cast<DRAWITEMSTRUCT*>(l));return TRUE;}
    if(msg==WM_CTLCOLORDLG)return reinterpret_cast<INT_PTR>(bgBrush);
    if(msg==WM_CTLCOLORSTATIC){SetTextColor((HDC)w,INK);SetBkColor((HDC)w,BG);return reinterpret_cast<INT_PTR>(bgBrush);}
    if(msg==WM_COMMAND&&LOWORD(w)==IDCANCEL){EndDialog(hwnd,0);return TRUE;}
    if(msg==WM_COMMAND&&(LOWORD(w)==703||LOWORD(w)==704)){try{if(LOWORD(w)==703){std::string key;{Guard guard(g.lock);if(g.relayMode=="ntfy")key=g.relayKey;}if(key.empty())key=RandomHex(32);ConfigureRelay("https://ntfy.sh",key,"ntfy");}else ConfigureRelay("","");EndDialog(hwnd,1);}catch(const Failure&f){MessageBoxW(hwnd,Wide(f.message).c_str(),L"设置未保存",MB_OK|MB_ICONINFORMATION);}return TRUE;}
    if(msg==WM_COMMAND&&LOWORD(w)==IDOK){wchar_t url[513]{},key[65]{};GetDlgItemTextW(hwnd,701,url,513);GetDlgItemTextW(hwnd,702,key,65);try{ConfigureRelay(Utf8(url),Utf8(key));SecureZeroMemory(key,sizeof(key));EndDialog(hwnd,1);}catch(const Failure&f){SecureZeroMemory(key,sizeof(key));MessageBoxW(hwnd,Wide(f.message).c_str(),L"设置未保存",MB_OK|MB_ICONINFORMATION);}return TRUE;}
    if(msg==WM_CLOSE){EndDialog(hwnd,0);return TRUE;}return FALSE;
}
static void ShowRemote(){struct {DLGTEMPLATE dlg;WORD menu,cls,title;} data{};data.dlg.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME;data.dlg.cx=367;data.dlg.cy=178;DialogBoxIndirectParamW(instance,&data.dlg,mainWindow,RemoteDialog,0);}
static void HandleCommand(int id){
    try{if(id==OPEN)ShowPanel();else if(id==RENEW){bool active;{Guard guard(g.lock);active=GetTickCount64()<g.codeExpires;}if(active)ClosePairing();else RenewCode();}else if(id==TOGGLE){if(listening)StopService();else if(!StartService())throw Failure{500,"无法开启服务，请检查端口是否被其他程序占用"};}
    else if(id==CANCEL)CancelPower();else if(id==CLEAR){if(MessageBoxW(mainWindow,L"清除所有已配对手机？之后需要重新输入配对码。",L"清除配对",MB_YESNO|MB_ICONQUESTION)==IDYES)ClearClients();}
    else if(id==EXIT)ExitApp();else if(id==REMOTE)ShowRemote();else if(id==FIREWALL)Firewall();else if(id==STARTUP){bool next=built?(SendMessageW(GetDlgItem(mainWindow,STARTUP),BM_GETCHECK,0,0)==BST_CHECKED):!StartupEnabled();if(!SetStartup(next))throw Failure{500,"无法更新开机自启设置"};}
    RefreshPanel();}catch(const Failure&f){MessageBoxW(mainWindow,Wide(f.message).c_str(),L"提示",MB_OK|MB_ICONINFORMATION);RefreshPanel();}catch(...){MessageBoxW(mainWindow,L"操作未完成，请重试。",L"提示",MB_OK|MB_ICONINFORMATION);}
}
static void AddTray(){tray={};tray.cbSize=sizeof(tray);tray.hWnd=mainWindow;tray.uID=1;tray.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;tray.uCallbackMessage=WM_TRAY;tray.hIcon=appIcon;wcscpy_s(tray.szTip,g.demo?L"关源 · 模拟模式":L"关源 · 双击打开");Shell_NotifyIconW(NIM_ADD,&tray);}
static void TrayMenu(){HMENU menu=CreatePopupMenu();AppendMenuW(menu,MF_STRING,OPEN,L"打开控制面板");AppendMenuW(menu,MF_STRING,CANCEL,L"取消待执行操作");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING|(StartupEnabled()?MF_CHECKED:0)|(g.demo?MF_GRAYED:0),STARTUP,L"开机自启");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING,EXIT,L"退出");POINT p;GetCursorPos(&p);SetForegroundWindow(mainWindow);int id=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,mainWindow,nullptr);DestroyMenu(menu);if(id==STARTUP){if(!SetStartup(!StartupEnabled()))MessageBoxW(mainWindow,L"无法更新开机自启设置。",L"提示",MB_OK|MB_ICONINFORMATION);RefreshPanel();}else if(id)HandleCommand(id);PostMessageW(mainWindow,WM_NULL,0,0);}
static LRESULT CALLBACK WindowProc(HWND hwnd,UINT message,WPARAM w,LPARAM l){
    if(taskbarCreated&&message==taskbarCreated){AddTray();return 0;}
    switch(message){
    case WM_NCCALCSIZE:return 0;
    case WM_NCACTIVATE:return TRUE;
    case WM_NCHITTEST:{
        POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        ScreenToClient(hwnd,&p);
        const RECT title=R(0,0,CaptionStart,TitleHeight);
        if(PtInRect(&title,p))return HTCAPTION;
        return HTCLIENT;
    }
    case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(hwnd,&ps);PaintBuffered(hwnd,dc);EndPaint(hwnd,&ps);return 0;}
    case WM_PRINTCLIENT:PaintBuffered(hwnd,(HDC)w);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_DRAWITEM:DrawButtonItem(*reinterpret_cast<DRAWITEMSTRUCT*>(l));return TRUE;
    case WM_CTLCOLORBTN:case WM_CTLCOLORSTATIC:SetTextColor((HDC)w,INK);SetBkColor((HDC)w,BG);return (LRESULT)bgBrush;
    case WM_COMMAND:
        if(HIWORD(w)==BN_CLICKED||l==0){
            const int id=LOWORD(w);
            if(id==CAPTION_MIN)SendMessageW(hwnd,WM_SYSCOMMAND,SC_MINIMIZE,0);
            else if(id==CAPTION_CLOSE)SendMessageW(hwnd,WM_CLOSE,0,0);
            else HandleCommand(id);
        }
        return 0;
    case WM_TIMER:if(w==1)RefreshPanel();else if(w==2)DestroyWindow(hwnd);return 0;
    case WM_UPDATE:RefreshPanel();return 0;
    case ShowExisting:ShowPanel();return 0;
    case QueryBuild:return DesktopBuild;
    case WM_TRAY:if(l==WM_LBUTTONDBLCLK||l==NIN_KEYSELECT)ShowPanel();else if(l==WM_RBUTTONUP||l==WM_CONTEXTMENU)TrayMenu();return 0;
    case WM_SIZE:if(w==SIZE_MINIMIZED)HidePanel();return 0;
    case WM_CLOSE:if(exiting)DestroyWindow(hwnd);else HidePanel();return 0;
    case WM_QUERYENDSESSION:try{CancelPower();}catch(...){}return TRUE;
    case WM_ENDSESSION:if(w)DestroyWindow(hwnd);return 0;
    case WM_DESTROY:KillTimer(hwnd,1);Shell_NotifyIconW(NIM_DELETE,&tray);PostQuitMessage(0);return 0;
    }return DefWindowProcW(hwnd,message,w,l);
}
static HFONT Font(int pixels,bool weight=false){return CreateFontW(-S(pixels),0,0,0,weight?FW_BOLD:FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");}
static void InitializeWindow(bool silent){
    SetProcessDPIAware();HDC screen=GetDC(nullptr);dpi=GetDeviceCaps(screen,LOGPIXELSX);ReleaseDC(nullptr,screen);bgBrush=CreateSolidBrush(BG);
    regular=Font(14);small=Font(12);bold=Font(15,true);titleFont=Font(32,true);codeFont=Font(39,true);captionFont=Font(22);
    appIcon=(HICON)LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,S(32),S(32),0);
    WNDCLASSEXW cls{sizeof(cls)};cls.lpfnWndProc=WindowProc;cls.hInstance=instance;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=appIcon;cls.hIconSm=appIcon;cls.hbrBackground=bgBrush;cls.lpszClassName=WindowClass;RegisterClassExW(&cls);
    DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN;
    int width=S(PanelWidth),height=S(PanelHeight);
    mainWindow=CreateWindowExW(0,cls.lpszClassName,g.demo?L"关源 v1.3.9（模拟）":L"关源 v1.3.9",style,(GetSystemMetrics(SM_CXSCREEN)-width)/2,(GetSystemMetrics(SM_CYSCREEN)-height)/2,width,height,nullptr,nullptr,instance,nullptr);
    if(!mainWindow)throw Failure{500,"无法创建控制面板"};
    if(!RoundWindow(mainWindow)){
        HRGN rounded=CreateRoundRectRgn(0,0,width+1,height+1,S(22),S(22));
        if(rounded&&!SetWindowRgn(mainWindow,rounded,FALSE))DeleteObject(rounded);
    }
    DwmSetWindowAttribute(mainWindow,DWMWA_BORDER_COLOR,&WINDOW_EDGE,sizeof(WINDOW_EDGE));
    taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");if(!preview)AddTray();if(!silent)ShowPanel();
}
static void DestroyUi(){if(mainWindow&&IsWindow(mainWindow))DestroyWindow(mainWindow);mainWindow=nullptr;ReleasePanelBuffer();if(heroIcon)DestroyIcon(heroIcon);if(appIcon)DestroyIcon(appIcon);for(HFONT font:{regular,small,bold,titleFont,codeFont,captionFont})if(font)DeleteObject(font);if(bgBrush)DeleteObject(bgBrush);}
static void SavePreview(const std::wstring&path){
    BuildPanel();HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);int width=S(760),height=S(680);BITMAPINFO bmi{};bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bmi.bmiHeader.biWidth=width;bmi.bmiHeader.biHeight=-height;bmi.bmiHeader.biPlanes=1;bmi.bmiHeader.biBitCount=32;bmi.bmiHeader.biCompression=BI_RGB;void*pixels=nullptr;HBITMAP bitmap=CreateDIBSection(screen,&bmi,DIB_RGB_COLORS,&pixels,nullptr,0);auto old=SelectObject(dc,bitmap);PaintPanel(dc);
    for(const auto& button:PanelButtons){
        HWND control=GetDlgItem(mainWindow,button.id);
        wchar_t label[128]{};
        GetWindowTextW(control,label,128);
        DrawButton(dc,R(button.x,button.y,button.width,button.height),button.id,label,IsWindowEnabled(control)?0:ODS_DISABLED);
    }
    RECT box=R(30,591,15,15);DrawFrameControl(dc,&box,DFC_BUTTON,DFCS_BUTTONCHECK|DFCS_INACTIVE);Text(dc,L"开机自启 · 登录 Windows 后静默驻留托盘",R(53,587,660,27),regular,MUTED);
    BITMAPFILEHEADER header{};header.bfType=0x4d42;header.bfOffBits=sizeof(header)+sizeof(BITMAPINFOHEADER);header.bfSize=header.bfOffBits+width*height*4;HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);DWORD written;
    if(file!=INVALID_HANDLE_VALUE){WriteFile(file,&header,sizeof(header),&written,nullptr);WriteFile(file,&bmi.bmiHeader,sizeof(BITMAPINFOHEADER),&written,nullptr);WriteFile(file,pixels,width*height*4,&written,nullptr);CloseHandle(file);}else throw Failure{500,"无法保存预览"};SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);ReleaseDC(nullptr,screen);
}
static void WriteText(const std::wstring&path,const std::string&text){HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)throw Failure{500,"无法写入测试报告"};DWORD n;bool ok=WriteFile(file,text.data(),(DWORD)text.size(),&n,nullptr)&&n==text.size();CloseHandle(file);if(!ok)throw Failure{500,"写入测试报告失败"};}
static bool IsCurrentBuild(HWND window){
    DWORD_PTR response=0;
    return window&&SendMessageTimeoutW(window,QueryBuild,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,500,&response)
        &&response==DesktopBuild;
}

static void OpenRunningInstance(){
    const uint64_t deadline=GetTickCount64()+3000;
    HWND window=nullptr;
    do{
        window=FindWindowW(WindowClass,nullptr);
        if(window)break;
        Sleep(50);
    }while(GetTickCount64()<deadline);
    if(IsCurrentBuild(window)){
        PostMessageW(window,ShowExisting,0,0);
        return;
    }
    MessageBoxW(nullptr,L"另一个关源实例正在运行。请右键任务栏托盘里的关源图标，选择「退出」，再打开 v1.3.9。\n\n关闭窗口只会收起到托盘。",L"关源 v1.3.9",MB_OK|MB_ICONINFORMATION);
}

static void RunUiTests(const std::wstring& path){
    std::string report;auto check=[&](bool ok,const char*label){if(!ok)throw Failure{500,std::string("FAIL: ")+label};report+="PASS: "+std::string(label)+"\n";};
    check(g.demo&&g.memoryOnly,"UI regression uses a mock executor and no saved configuration");
    check(GetConsoleWindow()==nullptr,"GUI executable has no attached console window");BuildPanel();
    HWND legacy=CreateWindowExW(0,L"STATIC",L"Legacy test window",WS_POPUP,0,0,0,0,nullptr,nullptr,instance,nullptr);
    check(legacy&&IsCurrentBuild(mainWindow)&&!IsCurrentBuild(legacy)&&!IsCurrentBuild(nullptr),"Only the current build may reopen through the single-instance message");
    DestroyWindow(legacy);
    RECT client;GetClientRect(mainWindow,&client);
    check(client.right==S(PanelWidth)&&client.bottom==S(PanelHeight),"Custom title bar fits in the original client area");
    auto hit=[&](POINT p){ClientToScreen(mainWindow,&p);return SendMessageW(mainWindow,WM_NCHITTEST,0,MAKELPARAM(p.x,p.y));};
    check(hit({S(200),S(23)})==HTCAPTION&&hit({S(680),S(23)})==HTCLIENT&&hit({S(100),S(85)})==HTCLIENT,
        "Only the title text area drags the window");
    RECT closeRect{};
    GetClientRect(GetDlgItem(mainWindow,CAPTION_CLOSE),&closeRect);
    check(closeRect.right>=S(44)&&closeRect.bottom>=S(38),"Caption buttons have larger click targets");
    int count=0;EnumThreadWindows(GetCurrentThreadId(),[](HWND w,LPARAM p)->BOOL{wchar_t cls[64];GetClassNameW(w,cls,64);if(!wcscmp(cls,L"PalmPower.NativeWindow"))++*(int*)p;return TRUE;},(LPARAM)&count);check(count==1,"Only one application main window exists");
    RefreshPanel();unsigned before=panelInvalidations;for(int i=0;i<20;i++)RefreshPanel();check(panelInvalidations==before,"Unchanged state does not repaint on idle refreshes");
    SchedulePower("shutdown",30);RefreshPanel();check(panelInvalidations==before+1,"A new countdown updates the displayed state");before=panelInvalidations;
    {Guard guard(g.lock);g.deadline=GetTickCount64()+29000;}RefreshPanel();check(panelInvalidations==before+1,"Changed countdown seconds refresh the display");CancelPower();RefreshPanel();
    ClosePairing();RefreshPanel();wchar_t label[32];GetDlgItemTextW(mainWindow,RENEW,label,32);check(!wcscmp(label,L"开始配对"),"Pairing is closed until the local start button is used");
    SendMessageW(GetDlgItem(mainWindow,RENEW),BM_CLICK,0,0);
    check(g.codeExpires>GetTickCount64(),"Rounded pairing button still opens the pairing window");
    ClosePairing();
    HDC screen=GetDC(mainWindow),direct=CreateCompatibleDC(screen),buffered=CreateCompatibleDC(screen);BITMAPINFO bmi{};bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bmi.bmiHeader.biWidth=S(760);bmi.bmiHeader.biHeight=-S(680);bmi.bmiHeader.biPlanes=1;bmi.bmiHeader.biBitCount=32;
    void*a=nullptr,*b=nullptr;HBITMAP first=CreateDIBSection(screen,&bmi,DIB_RGB_COLORS,&a,nullptr,0),second=CreateDIBSection(screen,&bmi,DIB_RGB_COLORS,&b,nullptr,0);HGDIOBJ oldA=SelectObject(direct,first),oldB=SelectObject(buffered,second);
    PaintPanel(direct);PaintBuffered(mainWindow,buffered);GdiFlush();check(!memcmp(a,b,(size_t)S(760)*S(680)*4),"Double-buffered frame matches the complete panel image");
    const DWORD handles=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    for(int i=0;i<100;i++)PaintBuffered(mainWindow,buffered);
    GdiFlush();
    const DWORD finalHandles=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    check(finalHandles<=handles+1,"Unchanged frames reuse their bitmap without acquiring drawing resources");
    RefreshPanel();
    SchedulePower("sleep",30);RefreshPanel();
    check(panelBufferDirty,"Power state changes invalidate the rendered background");
    PaintBuffered(mainWindow,buffered);GdiFlush();
    check(!panelBufferDirty&&memcmp(a,b,(size_t)S(760)*S(680)*4)!=0,"Repainting a countdown replaces the cached background");
    CancelPower();RefreshPanel();
    const int currentDpi=dpi;
    for(const int testDpi:{96,144}){
        dpi=testDpi;
        const RECT button=R(0,0,136,40);
        DrawButton(direct,button,TOGGLE,L"测试",0);
        GdiFlush();
        int blended=0;
        bool mirrored=true;
        for(int y=0;y<S(12);y++){
            for(int x=0;x<S(12);x++){
                const COLORREF pixel=GetPixel(direct,x,y);
                if(pixel!=BG&&pixel!=RED&&pixel!=CLR_INVALID)++blended;
                if(pixel!=GetPixel(direct,button.right-1-x,y))mirrored=false;
            }
        }
        check(GetPixel(direct,0,0)==BG&&GetPixel(direct,S(68),0)==RED&&blended>=8&&mirrored,
            testDpi==96?"Painted pill corners are smooth and symmetric at 100% DPI":"Painted pill corners are smooth and symmetric at 150% DPI");
    }
    dpi=currentDpi;
    const RECT focusButton=R(0,0,136,44);
    const HWND renew=GetDlgItem(mainWindow,RENEW);
    SendMessageW(mainWindow,WM_CHANGEUISTATE,MAKEWPARAM(UIS_CLEAR,UISF_HIDEFOCUS),0);
    SendMessageW(renew,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(S(20),S(20)));
    const bool hidesFocus=(SendMessageW(renew,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS)!=0;
    SendMessageW(renew,WM_LBUTTONUP,0,MAKELPARAM(-1,-1));
    check(hidesFocus,"Mouse input hides the keyboard focus indicator");
    DrawButton(direct,focusButton,CLEAR,L"清除配对",ODS_FOCUS|ODS_NOFOCUSRECT);
    DrawButton(buffered,focusButton,CLEAR,L"清除配对",0);
    GdiFlush();
    bool mouseMatches=true;
    const size_t stride=size_t(S(PanelWidth))*4;
    for(int y=0;y<focusButton.bottom;y++){
        if(memcmp(static_cast<const char*>(a)+y*stride,static_cast<const char*>(b)+y*stride,size_t(focusButton.right)*4))mouseMatches=false;
    }
    check(mouseMatches,"A mouse-focused button renders without a focus rectangle");
    DrawButton(direct,focusButton,CLEAR,L"清除配对",ODS_FOCUS);
    check(GetPixel(direct,S(68),0)==RED&&GetPixel(direct,0,0)==BG,
        "Keyboard focus uses a rounded outline");
    SelectObject(direct,oldA);SelectObject(buffered,oldB);DeleteObject(first);DeleteObject(second);DeleteDC(direct);DeleteDC(buffered);ReleaseDC(mainWindow,screen);
    SendMessageW(GetDlgItem(mainWindow,CAPTION_CLOSE),BM_CLICK,0,0);
    before=panelInvalidations;RefreshPanel();
    check(IsWindow(mainWindow)&&!built&&!panelBuffer&&panelInvalidations==before,"New close button hides to tray, stops refresh and releases controls and drawing buffer");
    BuildPanel();
    SendMessageW(GetDlgItem(mainWindow,CAPTION_MIN),BM_CLICK,0,0);
    check(IsWindow(mainWindow)&&!built,"New minimize button hides to tray without exiting");
    WriteText(path,report+"No real power, firewall or startup settings were changed.\n");
}
int WINAPI wWinMain(HINSTANCE app,HINSTANCE,LPWSTR,int){
    instance=app;int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);std::vector<std::wstring>args;for(int i=1;i<argc;i++)args.emplace_back(argv[i]);LocalFree(argv);
    auto has=[&](const wchar_t*flag){return std::find(args.begin(),args.end(),flag)!=args.end();};
    auto after=[&](const wchar_t*flag){auto p=std::find(args.begin(),args.end(),flag);return p!=args.end()&&p+1!=args.end()?*(p+1):std::wstring();};
    bool integration=has(L"--integration-server"),benchmark=has(L"--benchmark"),selfTest=has(L"--self-test");uiTesting=has(L"--ui-self-test");preview=has(L"--preview")||uiTesting;bool simulated=has(L"--simulate")||integration||benchmark||selfTest||preview;bool silent=has(L"--tray")||benchmark;
    HANDLE mutex=nullptr;int result=0;bool stateReady=false,networkReady=false;
    try{
        WSADATA winsock;if(WSAStartup(MAKEWORD(2,2),&winsock))throw Failure{500,"无法初始化网络"};networkReady=true;
        if(!integration&&!benchmark&&!selfTest&&!preview){
            wchar_t username[256]{};
            DWORD size=256;
            GetUserNameW(username,&size);
            const std::wstring key=L"Local\\PalmPower-"+std::wstring(username);
            mutex=CreateMutexW(nullptr,FALSE,key.c_str());
            if(!mutex)throw Failure{500,"无法创建程序实例"};
            if(GetLastError()==ERROR_ALREADY_EXISTS){
                if(!silent)OpenRunningInstance();
                CloseHandle(mutex);
                WSACleanup();
                return 0;
            }
        }
        InitializeState(simulated,simulated);stateReady=true;
        if((integration||benchmark)&&has(L"--test-relay-url"))ConfigureRelay(Utf8(after(L"--test-relay-url")),Utf8(after(L"--test-relay-key")),has(L"--test-relay-mode")?Utf8(after(L"--test-relay-mode")):"private");
        if(selfTest)result=RunSelfTests(after(L"--self-test"));
        else if(uiTesting){InitializeWindow(true);RunUiTests(after(L"--ui-self-test"));}
        else if(preview){InitializeWindow(true);SavePreview(after(L"--preview"));}
        else if(integration){
            if(!StartService(true,0,has(L"--integration-discovery")))throw Failure{500,"测试服务启动失败"};
            Json fixture=Json::obj();
            fixture["port"]=boundPort;
            fixture["discoveryPort"]=boundDiscoveryPort;
            fixture["code"]=g.code;
            fixture["info"]=Info();
            const std::wstring file=after(L"--integration-server");
            WriteText(file,fixture.dump());
            const uint64_t end=GetTickCount64()+90000;
            while(GetTickCount64()<end&&GetFileAttributesW((file+L".stop").c_str())==INVALID_FILE_ATTRIBUTES)Sleep(100);
        }
        else{
            InitializeWindow(silent);if(!StartService(benchmark,benchmark?0:18677,!benchmark))throw Failure{500,"无法开启局域网服务，请检查端口是否被占用"};
            if(benchmark){std::wstring file=after(L"--benchmark");if(has(L"--benchmark-panel")){SavePreview(file+L".bmp");HidePanel();DeleteFileW((file+L".bmp").c_str());}Json fixture=Json::obj();fixture["port"]=boundPort;fixture["code"]=g.code;fixture["info"]=Info();WriteText(file,fixture.dump());SetTimer(mainWindow,2,55000,nullptr);}
            MSG message;
            while(GetMessageW(&message,nullptr,0,0)>0){
                if(message.message==WM_KEYDOWN&&message.wParam==VK_TAB){
                    SendMessageW(mainWindow,WM_CHANGEUISTATE,MAKEWPARAM(UIS_CLEAR,UISF_HIDEFOCUS),0);
                }
                if(!IsDialogMessageW(mainWindow,&message)){
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
        }
    }catch(const Failure&error){result=1;std::wstring log=after(L"--self-test");if(log.empty())log=after(L"--ui-self-test");if(!log.empty())try{WriteText(log,"FAIL: "+error.message);}catch(...){}else MessageBoxW(nullptr,Wide(error.message).c_str(),L"关源",MB_OK|MB_ICONERROR);}
    catch(...){result=1;MessageBoxW(nullptr,L"无法启动，请检查程序文件和配对配置。",L"关源",MB_OK|MB_ICONERROR);}
    if(stateReady){StopService();DestroyUi();CleanupState();}if(mutex)CloseHandle(mutex);if(networkReady)WSACleanup();return result;
}
