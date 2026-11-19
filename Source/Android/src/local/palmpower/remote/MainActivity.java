package local.palmpower.remote;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputFilter;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.Window;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import org.json.JSONObject;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.HttpURLConnection;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicBoolean;

public class MainActivity extends Activity {
    private static final int BG=0xfffaf6f5, CARD=0xffffffff, INK=0xff2d2326, MUTED=0xff69575c;
    private static final int ACCENT=0xffa62539, TONAL=0xfff7e6e9, DISABLED=0xfff1eceb, DISABLED_INK=0xff776970;
    private final Handler main=new Handler(Looper.getMainLooper());
    private final ExecutorService io=Executors.newFixedThreadPool(3);
    private final LinkedHashMap<String,Device> devices=new LinkedHashMap<>();
    private final AtomicBoolean scanning=new AtomicBoolean();
    private SecurePrefs prefs;
    private String clientId;
    private LinearLayout page;
    private TextView searchNote,stateText,timeText,connectionDetail;
    private Button cancelButton;
    private final List<Button> powerButtons=new ArrayList<>();
    private Device selected;
    private boolean active,commandBusy,pollBusy,authFailed,connecting;
    private volatile boolean destroyed;
    private volatile DatagramSocket discoverySocket;
    private int scanGeneration;
    private long displayDeadline;
    private static class Device {
        String id,name,ip,modulus,exponent,fingerprint,relayUrl,relayKey,relayMode="private";
        volatile String session;
        final RemoteClock clock=new RemoteClock();
        boolean connected;
        int pairVersion;
        int port=18677;
        boolean sleep,hibernate,simulated,remote,relayCapable;
    }
    private interface Done { void accept(JSONObject data,Exception error); }
    private static class ApiError extends Exception {
        int status;
        ApiError(int code,String message) { super(message); status=code; }
    }
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        setTitle(R.string.app_name);
        setTaskDescription(new android.app.ActivityManager.TaskDescription(getString(R.string.app_name)));
        Window window=getWindow(); window.setStatusBarColor(BG); window.setNavigationBarColor(BG);
        // Keep controls clear of system bars on Android versions enforcing edge-to-edge layout.
        try{prefs=new SecurePrefs(this);}catch(RuntimeException ex){
            new AlertDialog.Builder(this).setTitle("安全存储不可用").setMessage("无法打开本机配对密钥。可以先重启手机重试；重置本机配对会删除这部手机保存的连接，需要重新配对电脑。").setPositiveButton("关闭",(d,w)->finish()).setNegativeButton("重置本机配对",(d,w)->{try{SecurePrefs.reset(this);recreate();}catch(Exception failure){new AlertDialog.Builder(this).setMessage("系统密钥库仍不可用，请检查手机系统状态。").setPositiveButton("关闭",(a,b)->finish()).show();}}).setCancelable(false).show();return;
        }
        clientId=prefs.getString("clientId",null);
        if(clientId==null) { clientId=Protocol.randomHex(16); prefs.edit().putString("clientId",clientId).apply(); }
        renderHome();
    }
    @Override protected void onResume() {
        super.onResume();if(prefs==null)return;active=true;
        if(selected==null) scan(); else poll();
    }
    @Override protected void onPause() {
        active=false; main.removeCallbacks(pollTask);main.removeCallbacks(countTask);
        DatagramSocket socket=discoverySocket; if(socket!=null) socket.close();
        super.onPause();
    }
    @Override protected void onDestroy() {
        destroyed=true; main.removeCallbacksAndMessages(null); io.shutdownNow(); super.onDestroy();
    }
    private int dp(int n) { return Math.round(n*getResources().getDisplayMetrics().density); }
    private GradientDrawable shape(int color,int radius) {
        GradientDrawable d=new GradientDrawable(); d.setColor(color); d.setCornerRadius(dp(radius)); return d;
    }
    private LinearLayout column() { LinearLayout l=new LinearLayout(this); l.setOrientation(LinearLayout.VERTICAL); return l; }
    private TextView text(String content,int size,int color,boolean bold) {
        TextView t=new TextView(this); t.setText(content); t.setTextSize(size); t.setTextColor(color);
        t.setLineSpacing(dp(3),1); if(bold)t.setTypeface(Typeface.DEFAULT,Typeface.BOLD); return t;
    }
    private void addText(LinearLayout parent,String value,int size,int color,boolean bold,int top) {
        TextView t=text(value,size,color,bold); LinearLayout.LayoutParams p=new LinearLayout.LayoutParams(-1,-2); p.topMargin=dp(top); parent.addView(t,p);
    }
    private Button actionButton(String title,boolean primary,View.OnClickListener click) {
        int[][] states={{-android.R.attr.state_enabled},{}};
        StateListDrawable fill=new StateListDrawable();
        fill.addState(states[0],shape(DISABLED,28));
        fill.addState(states[1],shape(primary?ACCENT:TONAL,28));

        Button button=new Button(this);
        button.setText(title);
        button.setTextSize(14);
        button.setAllCaps(false);
        button.setTypeface(Typeface.create("sans-serif-medium",Typeface.NORMAL));
        button.setTextColor(new ColorStateList(states,new int[]{DISABLED_INK,primary?CARD:ACCENT}));
        button.setBackgroundTintList(null);
        button.setBackground(new RippleDrawable(ColorStateList.valueOf(primary?0x33ffffff:0x22a62539),fill,shape(Color.WHITE,28)));
        button.setStateListAnimator(null);
        button.setElevation(0);
        button.setMinimumHeight(dp(48));
        button.setPadding(dp(16),dp(10),dp(16),dp(10));
        button.setOnClickListener(click);
        return button;
    }
    private Button button(LinearLayout parent,String title,boolean primary,View.OnClickListener click) {
        Button button=actionButton(title,primary,click);
        LinearLayout.LayoutParams params=new LinearLayout.LayoutParams(-1,-2);
        params.topMargin=dp(8);
        parent.addView(button,params);
        return button;
    }
    private LinearLayout card() {
        LinearLayout box=column();
        box.setPadding(dp(16),dp(16),dp(16),dp(16));
        box.setBackground(shape(CARD,24));
        LinearLayout.LayoutParams params=new LinearLayout.LayoutParams(-1,-2);
        params.topMargin=dp(12);
        page.addView(box,params);
        return box;
    }
    private void base(boolean controlPage) {
        LinearLayout root=column();
        root.setBackgroundColor(BG);
        root.setFitsSystemWindows(true);

        LinearLayout toolbar=new LinearLayout(this);
        toolbar.setGravity(Gravity.CENTER_VERTICAL);
        toolbar.setMinimumHeight(dp(56));
        toolbar.setPadding(dp(controlPage?4:20),0,dp(20),0);
        if(controlPage){
            Button back=actionButton("‹",false,v->back());
            back.setTextSize(29);
            back.setContentDescription("返回电脑列表");
            back.setPadding(0,0,0,0);
            back.setMinWidth(dp(48));
            back.setMinimumWidth(dp(48));
            back.setBackground(new RippleDrawable(ColorStateList.valueOf(0x22a62539),null,shape(Color.WHITE,24)));
            LinearLayout.LayoutParams backParams=new LinearLayout.LayoutParams(dp(48),dp(48));
            backParams.setMarginEnd(dp(8));
            toolbar.addView(back,backParams);
        }
        TextView title=text(controlPage?"电脑控制":"关源",22,INK,true);
        if(Build.VERSION.SDK_INT>=28)title.setAccessibilityHeading(true);
        toolbar.addView(title,new LinearLayout.LayoutParams(0,-2,1));
        root.addView(toolbar,new LinearLayout.LayoutParams(-1,-2));

        ScrollView scroll=new ScrollView(this);
        scroll.setFillViewport(true);
        page=column();
        page.setPadding(dp(16),dp(4),dp(16),dp(24));
        scroll.addView(page,new ScrollView.LayoutParams(-1,-2));
        root.addView(scroll,new LinearLayout.LayoutParams(-1,0,1));
        setContentView(root);
    }
    private void renderHome() {
        base(false);
        LinearLayout intro=card(); addText(intro,"查找电脑",18,INK,true,0);
        searchNote=text(scanning.get()?"正在搜索附近的电脑…":"自动发现局域网中的电脑",13,MUTED,false);
        LinearLayout.LayoutParams hint=new LinearLayout.LayoutParams(-1,-2); hint.topMargin=dp(8); intro.addView(searchNote,hint);
        LinearLayout discoveryActions=new LinearLayout(this);
        LinearLayout.LayoutParams actionGroup=new LinearLayout.LayoutParams(-1,-2);
        actionGroup.topMargin=dp(12);
        intro.addView(discoveryActions,actionGroup);
        Button rescan=actionButton(scanning.get()?"正在搜索…":"重新搜索",true,v->scan());
        rescan.setEnabled(!scanning.get());
        LinearLayout.LayoutParams scanWidth=new LinearLayout.LayoutParams(0,-1,1);
        scanWidth.setMarginEnd(dp(8));
        discoveryActions.addView(rescan,scanWidth);
        discoveryActions.addView(actionButton("手动输入 IP",false,v->manual()),new LinearLayout.LayoutParams(0,-1,1));
        for(Device device:devices.values()) {
            LinearLayout box=card();
            addText(box,device.name,18,INK,true,0);
            addText(box,device.ip+"  ·  "+(paired(device)?"已配对":"首次连接需要配对"),12,MUTED,false,8);
            button(box,paired(device)?"连接电脑":"连接并配对",true,v->connect(device));
        }
        for(String entry:prefs.getAll().keySet())if(entry.startsWith("remote_")){
            try{
                String id=entry.substring(7);if(devices.containsKey(id)||!paired(id))continue;
                JSONObject saved=new JSONObject(prefs.getString(entry,"{}"));Device d=new Device();d.id=id;d.name=saved.getString("name");d.remote=true;
                if(saved.optInt("securityVersion")!=2)continue;
                d.relayUrl=saved.getString("url");d.relayKey=saved.getString("key");d.relayMode=saved.optString("mode","private");d.sleep=saved.getBoolean("sleep");d.hibernate=saved.getBoolean("hibernate");d.simulated=saved.getBoolean("simulated");validateRemote(d.relayUrl,d.relayKey);
                LinearLayout box=card();addText(box,d.name,19,INK,true,0);addText(box,"已保存的远程入口 · Wi-Fi / 流量均可连接",12,MUTED,false,8);button(box,"通过网络连接",true,v->connect(d));
            }catch(Exception ignored){}
        }
        LinearLayout help=card();
        addText(help,"首次使用",15,INK,true,0);
        addText(help,"在电脑运行关源，允许局域网连接并点「开始配对」。手机上核对身份指纹，再输入配对码。",13,MUTED,false,8);
        addText(help,"使用手机流量前，请先在电脑开启跨网络，再用同一 Wi-Fi 连接一次。",13,MUTED,false,8);
    }
    private Network wifi() throws Exception {
        ConnectivityManager cm=(ConnectivityManager)getSystemService(CONNECTIVITY_SERVICE);
        for(Network n:cm.getAllNetworks()) {
            NetworkCapabilities caps=cm.getNetworkCapabilities(n);
            if(caps!=null && (caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)||caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET))) return n;
        }
        throw new Exception("请先让手机连接与电脑相同的 Wi-Fi");
    }
    private void scan() {
        if(!active || !scanning.compareAndSet(false,true)) return;
        final int generation=++scanGeneration;
        devices.clear(); if(selected==null)renderHome();
        io.execute(()->{
            WifiManager.MulticastLock lock=null;
            int found=0; Exception failure=null;
            try {
                Network net=wifi();
                WifiManager wm=(WifiManager)getApplicationContext().getSystemService(WIFI_SERVICE);
                if(wm!=null) { lock=wm.createMulticastLock("PalmPower discovery"); lock.setReferenceCounted(false); lock.acquire(); }
                Set<String> broadcasts=new LinkedHashSet<>(); broadcasts.add("255.255.255.255");
                ConnectivityManager cm=(ConnectivityManager)getSystemService(CONNECTIVITY_SERVICE);
                LinkProperties properties=cm.getLinkProperties(net);
                if(properties!=null) for(LinkAddress address:properties.getLinkAddresses()) {
                    if(!(address.getAddress() instanceof Inet4Address))continue;
                    int prefix=address.getPrefixLength(); if(prefix<1||prefix>30)continue;
                    byte[] raw=address.getAddress().getAddress(); int mask=-1<<(32-prefix);
                    int ip=((raw[0]&255)<<24)|((raw[1]&255)<<16)|((raw[2]&255)<<8)|(raw[3]&255);
                    int broadcast=ip|~mask;
                    broadcasts.add(((broadcast>>>24)&255)+"."+((broadcast>>>16)&255)+"."+((broadcast>>>8)&255)+"."+(broadcast&255));
                }
                try(DatagramSocket socket=new DatagramSocket(null)) {
                    discoverySocket=socket; socket.setReuseAddress(true); socket.bind(new InetSocketAddress(0)); net.bindSocket(socket);
                    socket.setBroadcast(true); socket.setSoTimeout(250);
                    byte[] query="PALM_POWER_DISCOVER_V1!".getBytes(StandardCharsets.US_ASCII);
                    long start=System.currentTimeMillis(),next=0;
                    Set<String> seen=new LinkedHashSet<>();
                    while(active && !destroyed && System.currentTimeMillis()-start<5500) {
                        if(System.currentTimeMillis()>=next) {
                            for(String dest:broadcasts) {
                                try { socket.send(new DatagramPacket(query,query.length,InetAddress.getByName(dest),18678)); } catch(Exception ignored) {}
                            }
                            next=System.currentTimeMillis()+1000;
                        }
                        byte[] bytes=new byte[2048]; DatagramPacket packet=new DatagramPacket(bytes,bytes.length);
                        try { socket.receive(packet); } catch(java.net.SocketTimeoutException timeout) { continue; }
                        try {
                            JSONObject result=new JSONObject(new String(packet.getData(),packet.getOffset(),packet.getLength(),StandardCharsets.UTF_8));
                            if(!"PalmPower".equals(result.optString("app")) || result.optInt("version")!=1)continue;
                            String id=result.getString("id"); if(!id.matches("[a-f0-9]{32}"))continue;
                            Device d=new Device(); d.id=id; d.name=result.getString("name"); d.ip=packet.getAddress().getHostAddress(); d.port=result.getInt("port");
                            if(d.name.length()>64 || d.port!=18677 || !(packet.getAddress() instanceof Inet4Address))continue;
                            if(seen.add(id))found++;
                            main.post(()->{ if(!destroyed&&generation==scanGeneration){devices.put(d.id,d);if(selected==null)renderHome();} });
                        } catch(Exception ignored) {}
                    }
                } finally { discoverySocket=null; }
            } catch(Exception ex) { if(active)failure=ex; }
            finally { if(lock!=null && lock.isHeld())lock.release(); }
            final int count=found; final Exception error=failure;
            main.post(()->{
                scanning.set(false); if(destroyed||generation!=scanGeneration)return;
                if(selected==null) { renderHome(); searchNote.setText(error!=null?friendly(error):count==0?"暂未发现电脑，请检查电脑端是否运行及网络放行。":"已发现 "+count+" 台电脑"); }
            });
        });
    }
    private void manual() {
        EditText input=new EditText(this); input.setTextColor(INK); input.setHint("例如 192.168.1.20"); input.setHintTextColor(MUTED);
        input.setInputType(InputType.TYPE_CLASS_PHONE); input.setSingleLine(true);
        AlertDialog dialog=new AlertDialog.Builder(this).setTitle("输入电脑端显示的 IP").setView(input).setNegativeButton("取消",null).setPositiveButton("连接",null).create();
        dialog.setOnShowListener(v->dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(b->{
            String ip=input.getText().toString().trim();
            if(!ip.matches("[0-9]{1,3}(\\.[0-9]{1,3}){3}")){input.setError("请输入 IPv4 地址");return;}
            for(String part:ip.split("\\."))if(Integer.parseInt(part)>255){input.setError("IP 地址无效");return;}
            Device d=new Device();d.ip=ip;d.name=ip;dialog.dismiss();connect(d);
        })); dialog.show();
    }
    private boolean paired(String id){return prefs.contains("key_"+id)&&"2".equals(prefs.getString("pair_version_"+id,"1"));}
    private boolean paired(Device d){return paired(d.id);}
    private void connect(Device d) {
        if(connecting)return;connecting=true;
        if(d.remote){connecting=false;selected=d;authFailed=false;renderControl();poll();return;}
        if(searchNote!=null)searchNote.setText("正在连接 "+d.name+"…");
        io.execute(()->{
            try {
                JSONObject info=http(d,"/info",null);
                if(!"PalmPower".equals(info.optString("app"))||info.optInt("version")!=1||!info.getString("id").matches("[a-f0-9]{32}"))throw new Exception("目标不是关源电脑端");
                if(d.id!=null && !d.id.equals(info.getString("id")))throw new Exception("电脑身份不一致，请重新搜索");
                d.id=info.getString("id");d.name=info.getString("name");d.modulus=info.getString("modulus");d.exponent=info.getString("exponent");
                d.fingerprint=Protocol.fingerprint(d.modulus,d.exponent,d.id);d.pairVersion=info.optInt("pairVersion");
                d.sleep=info.getBoolean("sleep");d.hibernate=info.getBoolean("hibernate");d.simulated=info.getBoolean("simulated");
                d.relayCapable=info.optBoolean("relayCapable");if(d.relayCapable&&paired(d))syncRemote(d);
                main.post(()->{connecting=false;if(destroyed)return;selected=d;authFailed=false;renderControl();if(!paired(d))pairDialog(d);else poll();});
            }catch(Exception ex){main.post(()->{connecting=false;showError(ex);});}
        });
    }
    private void pairDialog(Device d) {
        main.removeCallbacks(pollTask);
        if(d.pairVersion!=2){showError(new Exception("请更新电脑端后再配对，新版需要核对身份指纹"));return;}
        EditText input=new EditText(this);input.setTextColor(INK);input.setHint("电脑显示的 6 位数字");input.setHintTextColor(MUTED);
        input.setInputType(InputType.TYPE_CLASS_NUMBER);input.setFilters(new InputFilter[]{new InputFilter.LengthFilter(6)});input.setSingleLine(true);
        LinearLayout fields=column();fields.setPadding(dp(24),dp(8),dp(24),dp(8));addText(fields,Protocol.fingerprintLabel(d.fingerprint),16,ACCENT,true,0);CheckBox verified=new CheckBox(this);verified.setText("我已核对，与电脑显示的指纹一致");verified.setTextColor(INK);fields.addView(verified);fields.addView(input);
        AlertDialog dialog=new AlertDialog.Builder(this).setTitle("配对 "+d.name).setMessage("先比较两端的身份指纹。完全一致后，再输入电脑上的配对码；不一致请取消。").setView(fields).setNegativeButton("稍后",null).setPositiveButton("配对",null).create();
        dialog.setOnShowListener(v->dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(b->{
            if(!verified.isChecked()){verified.setError("请先核对身份指纹");return;}
            String pin=input.getText().toString();if(!pin.matches("[0-9]{6}")){input.setError("请输入 6 位配对码");return;}
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false);
            io.execute(()->{
                try {
                    String key=Protocol.newKey();String model=Build.MODEL;while(model.getBytes(StandardCharsets.UTF_8).length>24)model=model.substring(0,model.offsetByCodePoints(model.length(),-1));
                    JSONObject data=new JSONObject().put("i",clientId).put("n",model).put("s",key).put("p",pin).put("f",d.fingerprint);
                    while(data.toString().getBytes(StandardCharsets.UTF_8).length>190&&!model.isEmpty()){model=model.substring(0,model.offsetByCodePoints(model.length(),-1));data.put("n",model);}
                    JSONObject encrypted=new JSONObject().put("version",2).put("data",Protocol.encryptPair(data.toString(),d.modulus,d.exponent));
                    JSONObject result=http(d,"/pair",encrypted);
                    if(!result.optBoolean("ok")||!d.id.equals(result.optString("id")))throw new Exception("配对未完成");
                    if(!prefs.edit().putString("key_"+d.id,key).putString("pair_version_"+d.id,"2").commit())throw new Exception("手机无法保存配对，请重试");
                    if(d.relayCapable)syncRemote(d);
                    main.post(()->{if(destroyed)return;dialog.dismiss();authFailed=false;renderControl();poll();});
                }catch(Exception ex){main.post(()->{if(destroyed)return;dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(true);input.setError(friendly(ex));});}
            });
        }));dialog.show();
    }
    private void renderControl() {
        if(selected==null)return;
        Device d=selected;
        d.connected=false;powerButtons.clear();
        base(true);
        LinearLayout status=card();
        addText(status,d.name,22,INK,true,0);
        addText(status,(d.remote?"跨网络加密连接":"局域网 · "+d.ip)+(d.simulated?" · 模拟模式":""),13,MUTED,false,4);
        LinearLayout statusPanel=column();
        statusPanel.setBackground(shape(BG,18));
        statusPanel.setPadding(dp(14),dp(12),dp(14),dp(12));
        LinearLayout.LayoutParams statusParams=new LinearLayout.LayoutParams(-1,-2);
        statusParams.topMargin=dp(12);
        status.addView(statusPanel,statusParams);
        stateText=text(paired(d)?"正在获取状态…":"请先完成配对",15,INK,true);
        statusPanel.addView(stateText);
        timeText=text("—",26,ACCENT,true);
        statusPanel.addView(timeText);
        connectionDetail=text("",13,MUTED,false);
        statusPanel.addView(connectionDetail);
        cancelButton=button(status,"取消倒计时",false,v->send("cancel",0));cancelButton.setVisibility(View.GONE);
        if(d.simulated)addText(status,"当前操作只用于演练。",13,ACCENT,false,8);
        boolean paired=paired(d)&&!authFailed;
        if(!paired){if(d.remote)button(status,"返回列表，在同一 Wi-Fi 重新配对",true,v->back());else button(status,"输入配对码",true,v->{prefs.edit().remove("key_"+d.id).apply();connect(d);});}

        LinearLayout power=card();
        addText(power,"电源操作",16,INK,true,0);
        powerButtons.add(button(power,"关机",true,v->confirm("shutdown")));
        powerButtons.add(button(power,d.sleep?"睡眠 · 快速恢复":"睡眠 · 电脑当前不支持",false,v->confirm("sleep")));
        powerButtons.add(button(power,d.hibernate?"休眠 · 保存状态后断电":"休眠 · Windows 尚未启用",false,v->confirm("hibernate")));
        setConnectionReady(false);
        addText(power,"操作开始后有 30 秒倒计时，两端均可取消。关机前请保存文件。",13,MUTED,false,12);

        LinearLayout connection=card();
        addText(connection,"连接管理",16,INK,true,0);
        button(connection,"重新连接（查询状态）",false,v->poll());
        if(d.remote&&"ntfy".equals(d.relayMode)){
            button(connection,"检测手机网络",false,v->diagnose(d));
            addText(connection,"公共中转约每 30 秒更新状态。用完可返回列表，减少配额消耗。",13,MUTED,false,8);
        }
        if(d.remote)button(connection,"移除此远程入口",false,v->{prefs.edit().remove("remote_"+d.id).apply();back();});
    }
    private String label(String action){return "shutdown".equals(action)?"关机":"sleep".equals(action)?"睡眠":"休眠";}
    private void setConnectionReady(boolean ready){
        if(selected==null)return;selected.connected=ready;
        for(int i=0;i<powerButtons.size();i++){boolean enabled=ready&&!authFailed&&paired(selected)&&(i==0||i==1&&selected.sleep||i==2&&selected.hibernate);powerButtons.get(i).setEnabled(enabled);}
    }
    private void confirm(String action) {
        if(commandBusy||selected==null||!selected.connected)return;
        new AlertDialog.Builder(this).setTitle("确认让电脑"+label(action)+"？").setMessage(selected.name+" 将在 30 秒后"+label(action)+"。倒计时期间可以取消。").setNegativeButton("返回",null).setPositiveButton("确认"+label(action),(dialog,which)->send(action,30)).show();
    }
    private void updateState(JSONObject state) {
        if(selected==null||stateText==null)return;
        setConnectionReady(true);connectionDetail.setText(selected.remote?"已验证电脑回执 · "+networkLabel():"局域网连接正常");
        String action=state.optString("action");int seconds=state.optInt("remaining");
        stateText.setText(!action.isEmpty()?"即将"+label(action):state.optString("message","电脑已连接"));
        timeText.setText(action.isEmpty()?"已连接":seconds+"s");timeText.setTextSize(action.isEmpty()?26:44);
        cancelButton.setVisibility(action.isEmpty()?View.GONE:View.VISIBLE);
        main.removeCallbacks(countTask);displayDeadline=action.isEmpty()?0:android.os.SystemClock.elapsedRealtime()+seconds*1000L;if(displayDeadline>0)countTask.run();
    }
    private final Runnable countTask=new Runnable(){public void run(){if(!active||selected==null||displayDeadline==0||timeText==null)return;long remaining=Math.max(0,(displayDeadline-android.os.SystemClock.elapsedRealtime()+999)/1000);timeText.setText(remaining>0?remaining+"s":"正在提交…");if(remaining>0)main.postDelayed(this,1000);}};
    private void send(String action,int delay) {
        if(commandBusy||selected==null)return; commandBusy=true;Device d=selected;
        request(d,action,delay,(state,error)->{
            commandBusy=false;if(selected!=d)return;
            if(error!=null){
                if(error instanceof NtfyTransport.TransportError){setConnectionReady(false);connectionDetail.setText(friendly(error));if(((NtfyTransport.TransportError)error).published)error=new Exception(error.getMessage()+"\n电源请求可能已经送达。请先查询电脑状态，避免重复提交。");}
                showError(error);
            }else updateState(state);
        });
    }
    private final Runnable pollTask=()->poll();
    private void poll() {
        main.removeCallbacks(pollTask);
        if(!active||selected==null||authFailed||!paired(selected))return;
        if(pollBusy){main.postDelayed(pollTask,1000);return;}
        pollBusy=true;Device d=selected;
        request(d,"status",0,(data,error)->{
            pollBusy=false;if(selected!=d)return;
            if(error==null)updateState(data);
            else if(error instanceof ApiError && (((ApiError)error).status==403||((ApiError)error).status==426)) {
                authFailed=true;renderControl();stateText.setText(friendly(error));return;
            } else {setConnectionReady(false);stateText.setText("暂时连接不上电脑");connectionDetail.setText(friendly(error));timeText.setText("未连接");timeText.setTextSize(26);}
            if(active)main.postDelayed(pollTask,d.remote&&"ntfy".equals(d.relayMode)?30000:2000);
        });
    }
    private void request(Device d,String action,int delay,Done done) {
        io.execute(()->{
            JSONObject result=null;Exception failure=null;
            try {
                String key=prefs.getString("key_"+d.id,null);if(key==null)throw new Exception("请先配对");
                JSONObject command=command(key,action,delay);
                if(d.remote)result=remote(d,key,command);else result=http(d,"/command",command);
            }catch(Exception ex){failure=ex;}
            final JSONObject answer=result;final Exception error=failure;
            main.post(()->{if(!destroyed)done.accept(answer,error);});
        });
    }
    private JSONObject remote(Device d,String key,JSONObject command) throws Exception {
        return remote(d,key,command,false);
    }
    private JSONObject remote(Device d,String key,JSONObject command,boolean clockRetried) throws Exception {
        String action=command.getString("action");if(!action.equals("status")&&(d.session==null||!d.clock.ready(android.os.SystemClock.elapsedRealtime())))remote(d,key,command(key,"status",0));
        String nonce=command.getString("nonce");final long[] sentAt={0};NtfyTransport.Envelope build=()->{
            sentAt[0]=android.os.SystemClock.elapsedRealtime();long time=d.clock.seconds(sentAt[0],System.currentTimeMillis()/1000);
            // Re-sign at publication time using the authenticated peer-clock estimate.
            command.put("timestamp",time).put("signature",Protocol.sign(key,clientId,time,nonce,action,command.getInt("delay")));
            JSONObject payload=new JSONObject().put("version",2).put("session",d.session==null?"":d.session).put("expires",time+15).put("command",command);
            String[] encrypted=Protocol.seal(key,d.id,clientId,nonce,"request",payload.toString());return new JSONObject().put("clientId",clientId).put("requestId",nonce).put("iv",encrypted[0]).put("data",encrypted[1]).toString();
        };
        JSONObject answer="ntfy".equals(d.relayMode)?new JSONObject().put("envelope",new JSONObject(NtfyTransport.exchange(d.relayUrl,d.relayKey,clientId,nonce,build,remoteConnections(),stage->main.post(()->{if(selected==d&&connectionDetail!=null&&!destroyed)connectionDetail.setText(stage+"…");})))):http(d,"/v1/dispatch",new JSONObject().put("id",d.id).put("envelope",new JSONObject(build.build())));
        long receivedAt=android.os.SystemClock.elapsedRealtime();
        JSONObject decoded=new JSONObject(openEnvelope(key,d,nonce,"response",answer.getJSONObject("envelope")));JSONObject result=decoded.getJSONObject("result");int status=decoded.getInt("status");
        boolean clockHint=decoded.has("serverTime");if(clockHint){
            try{d.clock.observe(decoded.getLong("serverTime"),sentAt[0],receivedAt);}catch(IllegalArgumentException clockError){
                // A verified successful power response must still be shown as success,
                // even when the round trip was too slow to obtain a new clock sample.
                if(status!=200||action.equals("status"))throw clockError;
            }
        }
        if(RemoteClock.retryStatus(status,action,clockHint,clockRetried)){
            main.post(()->{if(selected==d&&connectionDetail!=null&&!destroyed)connectionDetail.setText("正在校准连接时间并重新查询状态…");});
            return remote(d,key,command(key,"status",0),true);
        }
        if(status!=200){if(status==409)d.session=null;if(status==408)throw new ApiError(status,clockHint?"请求时间校验失败，连接时间已校准。请点重新连接后再操作；本次请求未执行。":"电脑端尚不支持连接时间校准，请同时更新电脑端和手机至 v1.3.3。");throw new ApiError(status,result.optString("error","电脑拒绝了请求"));}
        String session=result.optString("session");if(!session.matches("[a-f0-9]{32}"))throw new Exception("电脑安全会话无效，请更新电脑端");d.session=session;return result;
    }
    private JSONObject command(String key,String action,int delay) throws Exception {
        long time=System.currentTimeMillis()/1000;String nonce=Protocol.randomHex(16);
        return new JSONObject().put("clientId",clientId).put("timestamp",time).put("nonce",nonce).put("action",action).put("delay",delay).put("signature",Protocol.sign(key,clientId,time,nonce,action,delay));
    }
    private String openEnvelope(String key,Device d,String nonce,String direction,JSONObject envelope) throws Exception {
        if(!clientId.equals(envelope.getString("clientId"))||!nonce.equals(envelope.getString("requestId")))throw new Exception("回执身份不匹配");
        return Protocol.open(key,d.id,clientId,nonce,direction,envelope.getString("iv"),envelope.getString("data"));
    }
    private void validateRemote(String address,String key) throws Exception {
        URL url=new URL(address);if(address.length()>512||!"https".equals(url.getProtocol())||url.getHost().isEmpty()||url.getUserInfo()!=null||url.getQuery()!=null||url.getRef()!=null||!key.matches("[0-9a-fA-F]{64}"))throw new Exception("远程设置无效，请在同一 Wi-Fi 重新同步");
    }
    private void syncRemote(Device d) throws Exception {
        String key=prefs.getString("key_"+d.id,null);JSONObject command=command(key,"relayConfig",0);JSONObject response=http(d,"/command",command);
        JSONObject config=new JSONObject(openEnvelope(key,d,command.getString("nonce"),"config",response.getJSONObject("envelope")));
        if(config.getString("url").isEmpty()){prefs.edit().remove("remote_"+d.id).commit();return;}
        if(config.optInt("securityVersion")!=2)throw new Exception("请更新电脑端后重新同步远程入口");
        validateRemote(config.getString("url"),config.getString("key"));
        if(!prefs.edit().putString("remote_"+d.id,config.toString()).commit())throw new Exception("无法保存远程入口");
    }
    private JSONObject http(Device d,String path,JSONObject body) throws Exception {
        HttpURLConnection conn;
        if(d.remote){validateRemote(d.relayUrl,d.relayKey);conn=remoteConnections().open(new URL(d.relayUrl.replaceAll("/+$","")+path));conn.setRequestProperty("Authorization","Bearer "+d.relayKey);}
        else {Network net=wifi();conn=(HttpURLConnection)net.openConnection(new URL("http://"+d.ip+":"+d.port+path));}
        conn.setConnectTimeout(d.remote?5000:3000);conn.setReadTimeout(d.remote?16000:5000);conn.setInstanceFollowRedirects(false);conn.setUseCaches(false);
        try {
            if(body!=null){conn.setRequestMethod("POST");conn.setDoOutput(true);conn.setRequestProperty("Content-Type","application/json; charset=utf-8");byte[] bytes=body.toString().getBytes(StandardCharsets.UTF_8);conn.setFixedLengthStreamingMode(bytes.length);try(OutputStream out=conn.getOutputStream()){out.write(bytes);}}
            int code=conn.getResponseCode();InputStream input=code>=400?conn.getErrorStream():conn.getInputStream();
            if(input==null)throw new Exception("电脑未返回数据");
            ByteArrayOutputStream buffer=new ByteArrayOutputStream();try(InputStream in=input){byte[] bytes=new byte[2048];int n;while((n=in.read(bytes))!=-1){if(buffer.size()+n>16384)throw new Exception("响应过大");buffer.write(bytes,0,n);}}
            JSONObject result=new JSONObject(new String(buffer.toByteArray(),StandardCharsets.UTF_8));
            if(code!=200){if(d.remote)throw new Exception(result.optString("error","中转连接失败，请检查电脑是否在线"));throw new ApiError(code,result.optString("error","电脑拒绝了请求"));}return result;
        }finally{conn.disconnect();}
    }
    private String friendly(Exception error) {
        if(error instanceof NtfyTransport.TransportError)return error.getMessage();
        if(selected!=null&&selected.remote&&error instanceof java.net.SocketTimeoutException)return "跨网连接超时，指令可能已经送达。请先查看电脑状态，再决定是否重试。";
        if(error instanceof java.net.SocketTimeoutException)return "连接超时，请检查电脑端是否运行，以及防火墙是否已放行";
        if(error instanceof java.net.ConnectException)return "无法连接，请检查电脑 IP 和电脑端服务";
        return error.getMessage()==null?"连接失败，请重新搜索":error.getMessage();
    }
    private NtfyTransport.Connections remoteConnections()throws Exception{
        ConnectivityManager cm=(ConnectivityManager)getSystemService(CONNECTIVITY_SERVICE);Network network=cm.getActiveNetwork();
        if(network==null)throw new Exception("手机没有可用网络，请检查移动数据及关源的联网权限");
        // Use sockets and DNS from this network, avoiding a connection left over from Wi-Fi.
        return url->(HttpURLConnection)network.openConnection(url);
    }
    private String networkLabel(){
        ConnectivityManager cm=(ConnectivityManager)getSystemService(CONNECTIVITY_SERVICE);Network n=cm.getActiveNetwork();NetworkCapabilities c=n==null?null:cm.getNetworkCapabilities(n);
        return c==null?"当前无可用网络":c.hasTransport(NetworkCapabilities.TRANSPORT_VPN)?"当前使用 VPN":c.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)?"当前使用移动数据":c.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)?"当前使用 Wi-Fi":"当前使用其他网络";
    }
    private void diagnose(Device d){
        if(selected!=d)return;connectionDetail.setText("正在检测中转收发，将发送一条随机测试消息…");
        io.execute(()->{String result;try{result=networkLabel()+"\n"+NtfyTransport.diagnose(d.relayUrl,remoteConnections());}catch(Exception ex){result=networkLabel()+"\n"+friendly(ex);}final String message=result;main.post(()->{if(selected==d&&!destroyed)connectionDetail.setText(message);});});
    }
    private void showError(Exception error) {
        if(destroyed||isFinishing())return;
        new AlertDialog.Builder(this).setTitle("提示").setMessage(friendly(error)).setPositiveButton("知道了",null).show();
    }
    private void back(){selected=null;authFailed=false;displayDeadline=0;main.removeCallbacks(countTask);main.removeCallbacks(pollTask);renderHome();if(!scanning.get())scan();}
    @Override public void onBackPressed(){if(selected!=null)back();else super.onBackPressed();}
}
