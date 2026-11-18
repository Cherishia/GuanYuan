import local.palmpower.remote.VaultCodec;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
public class VaultCodecTest {
    interface Attempt {void run() throws Exception;}
    static void check(boolean ok,String label){if(!ok)throw new AssertionError(label);System.out.println("PASS: "+label);}
    static boolean denied(Attempt run){try{run.run();return false;}catch(Exception expected){return true;}}
    public static void main(String[] args)throws Exception{
        KeyGenerator generator=KeyGenerator.getInstance("AES");generator.init(256);SecretKey key=generator.generateKey(),other=generator.generateKey();
        String value="pairing-secret-中文",stored=VaultCodec.encode(key,"device-key",value);
        check(VaultCodec.decode(key,"device-key",stored).equals(value),"Local encrypted storage Unicode round trip");
        check(!stored.equals(VaultCodec.encode(key,"device-key",value)),"Each storage write uses a fresh random IV");
        check(!stored.contains(value),"Stored record does not contain plaintext secret");
        check(denied(()->VaultCodec.decode(other,"device-key",stored)),"Wrong storage key refused");
        check(denied(()->VaultCodec.decode(key,"another-device",stored)),"Ciphertext cannot be moved to a different preference");
        String[] fields=stored.split(":");byte[] bytes=java.util.Base64.getDecoder().decode(fields[2]);bytes[0]^=1;
        String altered=fields[0]+":"+fields[1]+":"+java.util.Base64.getEncoder().encodeToString(bytes);
        check(denied(()->VaultCodec.decode(key,"device-key",altered)),"Modified local ciphertext refused");
        for(String malformed:new String[]{"plaintext","gks1::","gks1:?:?","gks0:a:b","gks1:"+"a".repeat(65536)})
            check(denied(()->VaultCodec.decode(key,"device-key",malformed)),"Malformed storage record refused");
        System.out.println("JVM codec tests only; physical Android Keystore behavior requires a phone.");
    }
}
