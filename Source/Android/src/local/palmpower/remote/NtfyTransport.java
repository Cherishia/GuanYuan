package local.palmpower.remote;

import java.net.HttpURLConnection;
import java.net.URL;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Base64;

/** Foreground-only encrypted request/reply over a public ntfy server. No SDK. */
public final class NtfyTransport {
    private NtfyTransport(){}
    public interface Connections {HttpURLConnection open(URL url) throws Exception;}
    public interface Progress {void update(String stage);}
    public interface Envelope {String build() throws Exception;}
    public static final class TransportError extends java.io.IOException {
        public final String stage;public final boolean published;
        TransportError(String phase,boolean sent,Exception cause){super(describe(phase,sent,cause),cause);stage=phase;published=sent;}
    }
    private static String describe(String stage,boolean published,Exception error){
        String reason;
        if(error instanceof java.net.UnknownHostException)reason="域名解析失败，请检查手机网络或私人 DNS";
        else if(error instanceof javax.net.ssl.SSLException)reason="HTTPS 校验失败，请检查系统时间、证书或网络代理";
        else if(error instanceof java.net.SocketTimeoutException)reason=stage.startsWith("检测中转")?"中转收发测试超时，尚不能确认这条网络可用于消息传递":published?"未收到电脑回执，请确认电脑在线并在同一 Wi-Fi 重新同步入口":"连接超时，请检查关源是否获准使用移动数据";
        else if(error instanceof java.net.ConnectException)reason="连接被拒绝或网络不可达，请检查移动数据权限、VPN 或代理";
        else reason=error.getMessage()==null?"连接中断":error.getMessage();
        return stage+"："+reason;
    }
    public static String requestTopic(String root) throws Exception {return topic(root,"GuanYuan","request","gyq_");}
    public static String responseTopic(String root,String client,String request) throws Exception {
        return topic(root,client+request,"response","gyr_");
    }
    private static String topic(String root,String identity,String action,String prefix) throws Exception {
        if(!root.matches("[a-f0-9A-F]{64}"))throw new IllegalArgumentException("远程入口无效");
        byte[] raw=new byte[32];for(int i=0;i<32;i++)raw[i]=(byte)Integer.parseInt(root.substring(i*2,i*2+2),16);
        return prefix+Protocol.sign(Base64.getEncoder().encodeToString(raw),identity,0,"ntfy",action,0).replace('+','-').replace('/','_').replace("=","");
    }
    private static HttpURLConnection open(String base,String path,int timeout,Connections connections) throws Exception {
        URL url=new URL(base.replaceAll("/+$","")+path);
        if(!"https".equals(url.getProtocol())||url.getUserInfo()!=null||url.getQuery()!=null||url.getRef()!=null)throw new IllegalArgumentException("公共中转必须使用 HTTPS");
        HttpURLConnection c=connections.open(url);c.setConnectTimeout(6000);c.setReadTimeout(timeout);c.setInstanceFollowRedirects(false);c.setUseCaches(false);c.setRequestProperty("Accept-Encoding","identity");return c;
    }
    private static void status(HttpURLConnection c) throws Exception {
        int code=c.getResponseCode();if(code!=200)throw new Exception("HTTP "+code+"，"+(code==429?"公共中转达到限额，请稍后再试":code==401||code==403?"公共中转拒绝访问":"公共中转连接失败"));
    }
    public static String exchange(String base,String root,String client,String request,String envelope) throws Exception {
        return exchange(base,root,client,request,()->envelope,url->(HttpURLConnection)url.openConnection(),stage->{});
    }
    public static String exchange(String base,String root,String client,String request,Envelope envelope,Connections connections,Progress progress) throws Exception {
        String stage="连接公共中转";boolean published=false;HttpURLConnection sub=null;
        try{
            progress.update(stage);String topic=requestTopic(root);sub=open(base,"/"+responseTopic(root,client,request)+"/raw",15000,connections);
            // Confirm the raw stream's open event before building the expiring request.
            // This also avoids spending command validity on the first TLS handshake.
            status(sub);InputStream stream=new BufferedInputStream(sub.getInputStream());
            String opening=readLine(stream,System.nanoTime()+15000000000L);if(!opening.isEmpty())throw new Exception("回执订阅握手无效");
            stage="发送加密请求";progress.update(stage);HttpURLConnection pub=open(base,"/"+topic,7000,connections);
            try{
                byte[] bytes=envelope.build().getBytes(StandardCharsets.UTF_8);if(bytes.length>4096)throw new IllegalArgumentException("指令过大");
                pub.setRequestMethod("POST");pub.setDoOutput(true);pub.setRequestProperty("Content-Type","text/plain; charset=utf-8");pub.setRequestProperty("Cache","no");pub.setRequestProperty("Firebase","no");pub.setFixedLengthStreamingMode(bytes.length);
                // Once transmission starts its delivery may be uncertain; never retry power automatically.
                try(OutputStream out=pub.getOutputStream()){published=true;out.write(bytes);}status(pub);
                try(InputStream ack=pub.getInputStream()){byte[] buffer=new byte[1024];int total=0,n;while((n=ack.read(buffer))!=-1){total+=n;if(total>8192)throw new Exception("公共服务响应过大");}}
            }finally{pub.disconnect();}
            stage="等待电脑回执";progress.update(stage);long end=System.nanoTime()+15000000000L;
            while(System.nanoTime()<end){String line=readLine(stream,end);if(!line.isEmpty())return line;}
            throw new java.net.SocketTimeoutException("未收到电脑回执");
        }catch(Exception error){throw new TransportError(stage,published,error);}finally{if(sub!=null)sub.disconnect();}
    }
    private static String readLine(InputStream in,long end)throws Exception{
        ByteArrayOutputStream line=new ByteArrayOutputStream();int c;
        while(System.nanoTime()<end&&(c=in.read())!=-1){if(c=='\n')return new String(line.toByteArray(),StandardCharsets.UTF_8);if(c=='\r')continue;if(line.size()>=8192)throw new Exception("回执过大");line.write(c);}
        if(System.nanoTime()>=end)throw new java.net.SocketTimeoutException("等待消息超时");
        throw new java.io.EOFException("公共中转提前关闭连接");
    }
    public static String diagnose(String base,Connections connections)throws Exception{
        // Test the same subscribe/publish APIs as real traffic, without depending on
        // an optional health endpoint and without using any device or pairing data.
        String topic="gyd_"+Protocol.randomHex(16),marker="GuanYuan connectivity "+Protocol.randomHex(16);
        String stage="检测中转订阅";boolean published=false;HttpURLConnection sub=null;
        try{
            sub=open(base,"/"+topic+"/raw",12000,connections);status(sub);
            InputStream in=new BufferedInputStream(sub.getInputStream());
            if(!readLine(in,System.nanoTime()+12000000000L).isEmpty())throw new Exception("订阅握手无效");
            stage="检测中转发送";HttpURLConnection pub=open(base,"/"+topic,7000,connections);
            try{
                byte[] data=marker.getBytes(StandardCharsets.UTF_8);pub.setRequestMethod("POST");pub.setDoOutput(true);pub.setRequestProperty("Content-Type","text/plain; charset=utf-8");pub.setRequestProperty("Cache","no");pub.setRequestProperty("Firebase","no");pub.setFixedLengthStreamingMode(data.length);
                try(OutputStream out=pub.getOutputStream()){published=true;out.write(data);}status(pub);
                try(InputStream ack=pub.getInputStream()){int total=0,n;byte[] b=new byte[1024];while((n=ack.read(b))!=-1){if((total+=n)>8192)throw new Exception("中转响应过大");}}
            }finally{pub.disconnect();}
            stage="检测中转接收";long end=System.nanoTime()+12000000000L;
            while(System.nanoTime()<end){String line=readLine(in,end);if(line.isEmpty())continue;if(marker.equals(line))return "手机与公共中转的发送、接收测试通过。这还不能证明电脑已收到指令；若连接电脑仍失败，请查看连接页的具体错误，并在同一 Wi-Fi 重新同步入口。";throw new Exception("随机测试消息校验失败");}
            throw new java.net.SocketTimeoutException("未收到随机测试消息");
        }catch(Exception error){throw new TransportError(stage,published,error);}finally{if(sub!=null)sub.disconnect();}
    }
}
