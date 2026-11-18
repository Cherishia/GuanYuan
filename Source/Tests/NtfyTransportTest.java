import local.palmpower.remote.NtfyTransport;
import java.io.*;
import java.net.*;
import javax.net.ssl.SSLHandshakeException;
public class NtfyTransportTest {
    static void check(boolean ok,String name){if(!ok)throw new AssertionError(name);System.out.println("PASS: "+name);}
    static String root="00".repeat(32),client="01".repeat(16),request="02".repeat(16);
    interface Attempt{void run()throws Exception;}
    static NtfyTransport.TransportError failed(Attempt attempt)throws Exception{try{attempt.run();throw new AssertionError("Expected failure");}catch(NtfyTransport.TransportError error){return error;}}
    static class Fake extends HttpURLConnection{
        int code=200;boolean disconnected;InputStream input;ByteArrayOutputStream output=new ByteArrayOutputStream();
        Fake(URL url,String body){super(url);input=new ByteArrayInputStream(body.getBytes(java.nio.charset.StandardCharsets.UTF_8));}
        public int getResponseCode(){return code;}public InputStream getInputStream(){return input;}public OutputStream getOutputStream(){return output;}
        public void disconnect(){disconnected=true;}public boolean usingProxy(){return false;}public void connect(){}
    }
    public static void main(String[] args)throws Exception{
        final boolean[] opened={false},built={false};final int[] posts={0},connections={0};final Fake[] last={null};
        NtfyTransport.Connections factory=url->{connections[0]++;if(url.getPath().endsWith("/raw")){
            Fake sub=new Fake(url,"\n{\"ok\":true}\n");sub.input=new FilterInputStream(sub.input){public int read()throws IOException{opened[0]=true;return super.read();}public int read(byte[]b,int o,int l)throws IOException{opened[0]=true;return super.read(b,o,l);}};return sub;
        }Fake post=new Fake(url,"{}");last[0]=post;posts[0]++;return post;};
        String answer=NtfyTransport.exchange("https://example.test",root,client,request,()->{check(opened[0],"Envelope built only after subscription readiness, preserving expiry for slow handshakes");built[0]=true;return "{\"test\":true}";},factory,stage->{});
        check(answer.equals("{\"ok\":true}")&&built[0],"Raw request and response complete");
        check(connections[0]==2&&posts[0]==1,"Selected network factory used for both connections without retry");
        check("no".equals(last[0].getRequestProperty("Cache"))&&"no".equals(last[0].getRequestProperty("Firebase")),"Publish disables caching and Firebase");
        check(last[0].disconnected,"Publish connection closed");
        NtfyTransport.TransportError dns=failed(()->NtfyTransport.exchange("https://example.test",root,client,request,()->"{}",url->{throw new UnknownHostException();},stage->{}));
        check(dns.getMessage().contains("域名解析失败")&&!dns.published,"DNS error identifies mobile-to-service failure before publication");
        NtfyTransport.TransportError tls=failed(()->NtfyTransport.exchange("https://example.test",root,client,request,()->"{}",url->{throw new SSLHandshakeException("test");},stage->{}));
        check(tls.getMessage().contains("HTTPS 校验失败")&&!tls.published,"TLS failure reported without disabling certificate verification");
        int[] deniedPosts={0};NtfyTransport.TransportError quota=failed(()->NtfyTransport.exchange("https://example.test",root,client,request,()->"{}",url->{Fake f=new Fake(url,url.getPath().endsWith("/raw")?"\n":"{}");if(!url.getPath().endsWith("/raw")){deniedPosts[0]++;f.code=429;}return f;},stage->{}));
        check(quota.getMessage().contains("HTTP 429")&&deniedPosts[0]==1,"Quota error remains visible and does not automatically retry a command");
        NtfyTransport.TransportError noReply=failed(()->NtfyTransport.exchange("https://example.test",root,client,request,()->"{}",url->{Fake f=new Fake(url,"\n");if(url.getPath().endsWith("/raw"))f.input=new InputStream(){int count;public int read()throws IOException{if(count++==0)return '\n';throw new SocketTimeoutException();}};return f;},stage->{}));
        check(noReply.published&&noReply.stage.equals("等待电脑回执"),"Timeout after publication distinguishes missing PC reply");
        final Fake[] diagnosisPost={null};java.util.List<String> paths=new java.util.ArrayList<>();
        String diagnosis=NtfyTransport.diagnose("https://example.test",url->{
            paths.add(url.getPath());Fake f=new Fake(url,"{}");if(url.getPath().endsWith("/raw")){
                InputStream echo=new InputStream(){byte[] data;int at;void prepare(){if(data==null)data=(new String(diagnosisPost[0].output.toByteArray(),java.nio.charset.StandardCharsets.UTF_8)+"\n").getBytes(java.nio.charset.StandardCharsets.UTF_8);}public int read(){prepare();return at==data.length?-1:data[at++]&255;}public int read(byte[] b,int o,int l){prepare();if(at==data.length)return -1;int n=Math.min(l,data.length-at);System.arraycopy(data,at,b,o,n);at+=n;return n;}};
                f.input=new SequenceInputStream(new ByteArrayInputStream(new byte[]{'\n'}),echo);
            }else diagnosisPost[0]=f;return f;
        });
        check(diagnosis.contains("发送、接收测试通过"),"Diagnosis verifies random marker publish and receive independently of PC");
        check(paths.size()==2&&paths.get(0).equals(paths.get(1)+"/raw")&&paths.get(1).startsWith("/gyd_"),"Diagnosis uses a fresh subscription topic and never requests optional health endpoint");
        check("no".equals(diagnosisPost[0].getRequestProperty("Cache"))&&"no".equals(diagnosisPost[0].getRequestProperty("Firebase")),"Diagnostic marker is published without cache or Firebase");
        NtfyTransport.TransportError unexpected=failed(()->NtfyTransport.diagnose("https://example.test",url->new Fake(url,"\nwrong-marker\n")));
        check(unexpected.getMessage().contains("随机测试消息校验失败"),"Unexpected diagnostic marker is rejected");
        System.out.println("Fake HTTP connections test logic only; HTTPS integration has a separate runner.");
    }
}
