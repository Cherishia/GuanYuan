import local.palmpower.remote.Protocol;
import local.palmpower.remote.NtfyTransport;
import local.palmpower.remote.RemoteClock;
import java.net.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.regex.*;

/** JVM cross-platform test for the exact Protocol.java compiled into the Android APK. */
public class ProtocolIntegration {
    static int port;
    static long wallSkew=0,originNanos=System.nanoTime(),sentAt;
    static RemoteClock clock=new RemoteClock();
    static long elapsed(){return (System.nanoTime()-originNanos)/1000000;}
    static long wall(){return System.currentTimeMillis()/1000+wallSkew;}
    static String client="0123456789abcdef0123456789abcdef",key,computer,relay,relayKey,session="";static boolean ntfy;
    static void check(boolean condition,String label) { if(!condition)throw new AssertionError(label);System.out.println("PASS: "+label); }
    static String string(String json,String name) { Matcher m=Pattern.compile("\""+name+"\":\"([^\"]*)\"").matcher(json);if(!m.find())throw new IllegalArgumentException(name);return m.group(1); }
    static class Result {int code;String body;Result(int c,String b){code=c;body=b;} }
    static Result http(String path,String body)throws Exception {
        HttpURLConnection c=(HttpURLConnection)new URL("http://127.0.0.1:"+port+path).openConnection(Proxy.NO_PROXY);c.setConnectTimeout(3000);c.setReadTimeout(5000);
        try {
            if(body!=null){c.setRequestMethod("POST");c.setDoOutput(true);c.setRequestProperty("Content-Type","application/json");byte[] b=body.getBytes(StandardCharsets.UTF_8);c.setFixedLengthStreamingMode(b.length);try(OutputStream s=c.getOutputStream()){s.write(b);}}
            int code=c.getResponseCode();InputStream in=code>=400?c.getErrorStream():c.getInputStream();
            ByteArrayOutputStream out=new ByteArrayOutputStream();try(InputStream s=in){byte[] b=new byte[1024];int n;while((n=s.read(b))!=-1)out.write(b,0,n);}return new Result(code,new String(out.toByteArray(),StandardCharsets.UTF_8));
        }finally{c.disconnect();}
    }
    static String command(String action,int delay)throws Exception {
        return command(action,delay,wall());
    }
    static String command(String action,int delay,long time)throws Exception {
        String nonce=Protocol.randomHex(16);
        return "{\"clientId\":\""+client+"\",\"timestamp\":"+time+",\"nonce\":\""+nonce+"\",\"action\":\""+action+"\",\"delay\":"+delay+",\"signature\":\""+Protocol.sign(key,client,time,nonce,action,delay)+"\"}";
    }
    static Result remote(String command,long expires,boolean tamper)throws Exception {
        sentAt=elapsed();
        String nonce=string(command,"nonce");String[] sealed=Protocol.seal(key,computer,client,nonce,"request","{\"version\":2,\"session\":\""+session+"\",\"expires\":"+expires+",\"command\":"+command+"}");
        if(tamper){byte[] raw=java.util.Base64.getDecoder().decode(sealed[1]);raw[0]^=1;sealed[1]=java.util.Base64.getEncoder().encodeToString(raw);}
        String envelope="{\"clientId\":\""+client+"\",\"requestId\":\""+nonce+"\",\"iv\":\""+sealed[0]+"\",\"data\":\""+sealed[1]+"\"}";
        if(ntfy)return decode(NtfyTransport.exchange(relay,relayKey,client,nonce,envelope),nonce);
        HttpURLConnection c=(HttpURLConnection)new URL(relay+"/v1/dispatch").openConnection(Proxy.NO_PROXY);c.setConnectTimeout(3000);c.setReadTimeout(16000);c.setRequestMethod("POST");c.setDoOutput(true);c.setRequestProperty("Content-Type","application/json");c.setRequestProperty("Authorization","Bearer "+relayKey);
        try{
            byte[] bytes=("{\"id\":\""+computer+"\",\"envelope\":"+envelope+"}").getBytes(StandardCharsets.UTF_8);c.setFixedLengthStreamingMode(bytes.length);try(OutputStream out=c.getOutputStream()){out.write(bytes);}
            if(c.getResponseCode()!=200)throw new AssertionError("Relay HTTP "+c.getResponseCode());
            String response=new String(c.getInputStream().readAllBytes(),StandardCharsets.UTF_8);
            return decode(response,nonce);
        }finally{c.disconnect();}
    }
    static Result decode(String response,String nonce)throws Exception {
        check(string(response,"clientId").equals(client)&&string(response,"requestId").equals(nonce),"Response bound to requesting client and nonce");
        String plain=Protocol.open(key,computer,client,nonce,"response",string(response,"iv"),string(response,"data"));
        Matcher hint=Pattern.compile("\"serverTime\":(\\d+)").matcher(plain);if(hint.find())clock.observe(Long.parseLong(hint.group(1)),sentAt,elapsed());
        Matcher m=Pattern.compile("\"status\":(\\d+)").matcher(plain);m.find();int code=Integer.parseInt(m.group(1));if(code==200)session=string(plain,"session");return new Result(code,plain);
    }
    static Result remote(String action,int delay)throws Exception{long time=clock.seconds(elapsed(),wall());return remote(command(action,delay,time),time+15,false);}
    public static void main(String[] args)throws Exception {
        String fixture=new String(Files.readAllBytes(Paths.get(args[0])),StandardCharsets.UTF_8);
        Matcher m=Pattern.compile("\"port\":(\\d+)").matcher(fixture);m.find();port=Integer.parseInt(m.group(1));
        String zero=java.util.Base64.getEncoder().encodeToString(new byte[32]);
        check(Protocol.sign(zero,client,1700000000,"00112233445566778899aabbccddeeff","shutdown",30).equals("bybODxiv1AygyDt5g4ylNqIBdA+SBCjPuNRCxCTBYiQ="),"Java HMAC matches Python and C# reference");
        Result info=http("/info",null);check(info.code==200,"Java connects to Windows server");key=Protocol.newKey();
        computer=string(info.body,"id");
        String fingerprint=Protocol.fingerprint(string(info.body,"modulus"),string(info.body,"exponent"),computer);
        String pair="{\"f\":\""+fingerprint+"\",\"i\":\""+client+"\",\"n\":\"Android JVM test\",\"s\":\""+key+"\",\"p\":\""+string(fixture,"code")+"\"}";
        String encrypted=Protocol.encryptPair(pair,string(info.body,"modulus"),string(info.body,"exponent"));
        check(http("/pair","{\"version\":2,\"data\":\""+encrypted+"\"}").code==200,"Java OAEP encrypted pairing accepted by native Windows server");
        check(http("/command",command("status",0)).code==200,"Android signed status accepted");
        String request=command("shutdown",30);Result scheduled=http("/command",request);
        check(scheduled.code==200&&scheduled.body.contains("\"action\":\"shutdown\""),"Android schedules mock shutdown");
        check(http("/command",request).code==409,"Captured Android request cannot be replayed");
        Result cancelled=http("/command",command("cancel",0));check(cancelled.code==200&&cancelled.body.contains("\"action\":\"\""),"Android cancels countdown");
        for(String action:new String[]{"sleep","hibernate"}){check(http("/command",command(action,30)).code==200,"Android schedules mock "+action);check(http("/command",command("cancel",0)).code==200,"Android cancels "+action);}
        if(args.length>=3){
            relay=args[1];relayKey=args[2];ntfy=args.length>=4&&args[3].equals("ntfy");String configRequest=command("relayConfig",0);Result config=http("/command",configRequest);
            String decoded=Protocol.open(key,computer,client,string(configRequest,"nonce"),"config",string(config.body,"iv"),string(config.body,"data"));
            check(decoded.contains(args.length>=5?args[4]:relay)&&decoded.contains(relayKey)&&!config.body.contains(relayKey),"Remote configuration synced encrypted over LAN");
            boolean rejected=false;try{Protocol.open(key,computer,client,string(configRequest,"nonce"),"response",string(config.body,"iv"),string(config.body,"data"));}catch(Exception ex){rejected=true;}check(rejected,"Encrypted configuration cannot be substituted for power response");
            System.out.println("Computer relay state: "+string(http("/command",command("status",0)).body,"relayStatus"));
            check(remote("status",0).code==200,"Java Android AES-GCM request and native response interoperate through relay");
            for(long skew:new long[]{8,-86400,31536000}){
                wallSkew=skew;clock=new RemoteClock();Result skewed=remote("status",0);
                check(skewed.code==408&&RemoteClock.retryStatus(skewed.code,"status",true,false),"Clock skew "+skew+"s reproduces expiry rejection and obtains authenticated clock hint");
                check(remote("status",0).code==200,"Status succeeds after authenticated clock correction for skew "+skew+"s");
                check(remote("sleep",30).code==200&&remote("cancel",0).code==200,"Corrected timestamp and expiry allow mock power and cancellation for skew "+skew+"s");
            }wallSkew=0;
            String currentSession=session;session="00000000000000000000000000000000";
            check(remote("shutdown",30).code==409,"Power command with stale boot session refused");session=currentSession;
            check(remote("shutdown",10).code==400,"Internet power command below 30 seconds refused");
            check(remote("format",30).code==400,"Internet arbitrary command refused");
            String shutdown=command("shutdown",30);Result result=remote(shutdown,System.currentTimeMillis()/1000+15,false);check(result.code==200&&result.body.contains("\"action\":\"shutdown\""),"Relay schedules mock shutdown");
            check(remote(shutdown,System.currentTimeMillis()/1000+15,false).code==409,"Relay captured command cannot execute twice");
            check(remote("cancel",0).body.contains("\"action\":\"\""),"Relay cancellation clears countdown");
            check(remote(command("shutdown",30),System.currentTimeMillis()/1000-1,false).code==408,"Expired internet command is never scheduled");
            check(remote(command("shutdown",30),System.currentTimeMillis()/1000+15,true).code==403,"Altered encrypted internet command rejected");
            check(remote("status",0).body.contains("\"action\":\"\""),"Rejected commands leave power state unchanged");
            check(remote("relayConfig",0).code==403,"Relay cannot export computer connection credentials");
            for(String action:new String[]{"sleep","hibernate"}){check(remote(action,30).code==200,"Relay schedules mock "+action);check(remote("cancel",0).code==200,"Relay cancels "+action);}
        }
        System.out.println("No actual shutdown, sleep, or hibernation was performed.");
    }
}
