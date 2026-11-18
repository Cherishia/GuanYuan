import local.palmpower.remote.NtfyTransport;
/** Uses only a random echo marker, never a power command or a personal device key. */
public class NtfyPublicProbe {
    public static void main(String[] args)throws Exception{
        String body="{\"probe\":\""+args[3]+"\"}";
        String response=NtfyTransport.exchange("https://ntfy.sh",args[0],args[1],args[2],()->body,url->(java.net.HttpURLConnection)url.openConnection(),stage->System.out.println("STAGE: "+stage));
        if(!body.equals(response))throw new AssertionError("Echo mismatch");
        System.out.println("PASS: Real ntfy HTTPS request/reply using exact Android transport and random test data");
    }
}
