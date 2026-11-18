import local.palmpower.remote.NtfyTransport;
public class NtfyDiagnosticIntegration {
    public static void main(String[] args)throws Exception {
        String result=NtfyTransport.diagnose(args[0],url->(java.net.HttpURLConnection)url.openConnection());
        if(!result.contains("发送、接收测试通过"))throw new AssertionError(result);
        System.out.println("PASS: HTTPS diagnostic subscribe/publish/receive works with health endpoint absent (HTTP 404)");
    }
}
