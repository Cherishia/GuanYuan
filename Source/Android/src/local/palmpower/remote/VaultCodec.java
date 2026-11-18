package local.palmpower.remote;
import java.nio.charset.StandardCharsets;
import java.util.Base64;
import javax.crypto.Cipher;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/** Local storage format. The wrapping key is supplied by Android Keystore. */
public final class VaultCodec {
    private VaultCodec(){}
    public static String encode(SecretKey key,String name,String value) throws Exception {
        Cipher cipher=Cipher.getInstance("AES/GCM/NoPadding");cipher.init(Cipher.ENCRYPT_MODE,key);
        cipher.updateAAD(("GuanYuan/local/v1\n"+name).getBytes(StandardCharsets.UTF_8));
        return "gks1:"+Base64.getEncoder().encodeToString(cipher.getIV())+":"+Base64.getEncoder().encodeToString(cipher.doFinal(value.getBytes(StandardCharsets.UTF_8)));
    }
    public static String decode(SecretKey key,String name,String stored) throws Exception {
        String[] fields=stored.split(":",-1);if(fields.length!=3||!fields[0].equals("gks1")||stored.length()>65536)throw new IllegalArgumentException("安全存储格式无效");
        byte[] iv=Base64.getDecoder().decode(fields[1]),data=Base64.getDecoder().decode(fields[2]);if(iv.length!=12||data.length<16)throw new IllegalArgumentException("安全存储数据无效");
        Cipher cipher=Cipher.getInstance("AES/GCM/NoPadding");cipher.init(Cipher.DECRYPT_MODE,key,new GCMParameterSpec(128,iv));
        cipher.updateAAD(("GuanYuan/local/v1\n"+name).getBytes(StandardCharsets.UTF_8));return new String(cipher.doFinal(data),StandardCharsets.UTF_8);
    }
}
