package local.palmpower.remote;

import java.nio.charset.StandardCharsets;
import java.security.KeyFactory;
import java.security.SecureRandom;
import java.security.MessageDigest;
import java.security.spec.MGF1ParameterSpec;
import java.security.spec.RSAPublicKeySpec;
import java.math.BigInteger;
import java.util.Base64;
import javax.crypto.Cipher;
import javax.crypto.Mac;
import javax.crypto.spec.OAEPParameterSpec;
import javax.crypto.spec.PSource;
import javax.crypto.spec.SecretKeySpec;
import javax.crypto.spec.GCMParameterSpec;

public final class Protocol {
    private static final SecureRandom RANDOM = new SecureRandom();
    private Protocol() {}
    public static String randomHex(int count) {
        byte[] bytes = new byte[count]; RANDOM.nextBytes(bytes);
        StringBuilder text = new StringBuilder();
        for (byte b : bytes) text.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
        return text.toString();
    }
    public static String newKey() {
        byte[] bytes = new byte[32]; RANDOM.nextBytes(bytes);
        return Base64.getEncoder().encodeToString(bytes);
    }
    public static String fingerprint(String modulus,String exponent,String computer) throws Exception {
        return Base64.getEncoder().encodeToString(MessageDigest.getInstance("SHA-256").digest(("GuanYuan/pair/v2\n"+computer+"\n"+modulus+"\n"+exponent).getBytes(StandardCharsets.UTF_8)));
    }
    public static String fingerprintLabel(String value) {
        byte[] digest=Base64.getDecoder().decode(value);StringBuilder out=new StringBuilder();
        for(int i=0;i<10;i++){if(i>0&&i%2==0)out.append(' ');out.append(String.format(java.util.Locale.ROOT,"%02X",digest[i]&255));}return out.toString();
    }
    public static String sign(String key, String clientId, long timestamp, String nonce, String action, int delay) throws Exception {
        String canonical = clientId + "\n" + timestamp + "\n" + nonce + "\n" + action + "\n" + delay;
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(Base64.getDecoder().decode(key), "HmacSHA256"));
        return Base64.getEncoder().encodeToString(mac.doFinal(canonical.getBytes(StandardCharsets.UTF_8)));
    }
    public static String encryptPair(String json, String modulus, String exponent) throws Exception {
        byte[] plain = json.getBytes(StandardCharsets.UTF_8);
        if (plain.length > 190) throw new IllegalArgumentException("设备名称过长");
        RSAPublicKeySpec spec = new RSAPublicKeySpec(new BigInteger(1, Base64.getDecoder().decode(modulus)), new BigInteger(1, Base64.getDecoder().decode(exponent)));
        Cipher cipher = Cipher.getInstance("RSA/ECB/OAEPWithSHA-256AndMGF1Padding");
        cipher.init(Cipher.ENCRYPT_MODE, KeyFactory.getInstance("RSA").generatePublic(spec), new OAEPParameterSpec("SHA-256", "MGF1", MGF1ParameterSpec.SHA256, PSource.PSpecified.DEFAULT));
        return Base64.getEncoder().encodeToString(cipher.doFinal(plain));
    }
    private static byte[] aad(String computer,String client,String request,String direction) {
        if(!computer.matches("[a-f0-9]{32}")||!client.matches("[a-f0-9]{32}")||!request.matches("[a-f0-9]{32}"))throw new IllegalArgumentException("消息标识无效");
        return ("GuanYuan/v1\n"+computer+"\n"+client+"\n"+request+"\n"+direction).getBytes(StandardCharsets.UTF_8);
    }
    public static String[] seal(String key,String computer,String client,String request,String direction,String plain) throws Exception {
        byte[] iv=new byte[12];RANDOM.nextBytes(iv);Cipher cipher=Cipher.getInstance("AES/GCM/NoPadding");
        byte[] raw=Base64.getDecoder().decode(key);if(raw.length!=32)throw new IllegalArgumentException("配对密钥无效");
        cipher.init(Cipher.ENCRYPT_MODE,new SecretKeySpec(raw,"AES"),new GCMParameterSpec(128,iv));cipher.updateAAD(aad(computer,client,request,direction));
        return new String[]{Base64.getEncoder().encodeToString(iv),Base64.getEncoder().encodeToString(cipher.doFinal(plain.getBytes(StandardCharsets.UTF_8)))};
    }
    public static String open(String key,String computer,String client,String request,String direction,String iv,String data) throws Exception {
        byte[] nonce=Base64.getDecoder().decode(iv),ciphertext=Base64.getDecoder().decode(data),raw=Base64.getDecoder().decode(key);
        if(nonce.length!=12||ciphertext.length<16||ciphertext.length>8192||raw.length!=32)throw new IllegalArgumentException("加密消息无效");
        Cipher cipher=Cipher.getInstance("AES/GCM/NoPadding");cipher.init(Cipher.DECRYPT_MODE,new SecretKeySpec(raw,"AES"),new GCMParameterSpec(128,nonce));
        cipher.updateAAD(aad(computer,client,request,direction));return new String(cipher.doFinal(ciphertext),StandardCharsets.UTF_8);
    }
}
