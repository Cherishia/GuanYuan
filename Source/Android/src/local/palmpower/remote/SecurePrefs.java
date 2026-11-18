package local.palmpower.remote;
import android.content.Context;
import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import java.security.KeyStore;
import java.util.Map;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;

/** No plaintext fallback. Legacy strings migrate in one committed transaction. */
public final class SecurePrefs {
    private static final String ALIAS="guanyuan-storage-v1";
    private final SharedPreferences prefs;
    private final SecretKey key;
    public SecurePrefs(Context context){
        prefs=context.getSharedPreferences("pairing",Context.MODE_PRIVATE);
        try{
            KeyStore store=KeyStore.getInstance("AndroidKeyStore");store.load(null);
            if(!store.containsAlias(ALIAS)){
                KeyGenerator gen=KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES,"AndroidKeyStore");
                gen.init(new KeyGenParameterSpec.Builder(ALIAS,KeyProperties.PURPOSE_ENCRYPT|KeyProperties.PURPOSE_DECRYPT).setKeySize(256).setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).setRandomizedEncryptionRequired(true).build());gen.generateKey();
            }
            key=(SecretKey)store.getKey(ALIAS,null);if(key==null)throw new SecurityException("安全存储密钥不可用");
            SharedPreferences.Editor migration=prefs.edit();boolean changed=false;
            for(Map.Entry<String,?> e:prefs.getAll().entrySet())if(e.getValue() instanceof String){String value=(String)e.getValue();if(value.startsWith("gks1:"))VaultCodec.decode(key,e.getKey(),value);else{migration.putString(e.getKey(),VaultCodec.encode(key,e.getKey(),value));changed=true;}}
            if(changed&&!migration.commit())throw new SecurityException("无法保存安全存储迁移");
        }catch(Exception ex){throw new SecurityException("无法打开本机安全存储",ex);}
    }
    public boolean contains(String name){return prefs.contains(name);}
    public Map<String,?> getAll(){return prefs.getAll();}
    public String getString(String name,String fallback){String stored=prefs.getString(name,null);if(stored==null)return fallback;try{return VaultCodec.decode(key,name,stored);}catch(Exception ex){throw new SecurityException("无法解密本机配对记录",ex);}}
    public Editor edit(){return new Editor(prefs.edit());}
    public final class Editor {
        private final SharedPreferences.Editor editor;
        Editor(SharedPreferences.Editor value){editor=value;}
        public Editor putString(String name,String value){try{editor.putString(name,VaultCodec.encode(key,name,value));return this;}catch(Exception ex){throw new SecurityException("无法加密配对记录",ex);}}
        public Editor remove(String name){editor.remove(name);return this;}
        public boolean commit(){return editor.commit();}
        public void apply(){editor.apply();}
    }
    public static void reset(Context context) throws Exception {
        KeyStore store=KeyStore.getInstance("AndroidKeyStore");store.load(null);store.deleteEntry(ALIAS);
        if(!context.getSharedPreferences("pairing",Context.MODE_PRIVATE).edit().clear().commit())throw new SecurityException("无法重置本机配对");
    }
}
