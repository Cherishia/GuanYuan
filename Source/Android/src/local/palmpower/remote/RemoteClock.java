package local.palmpower.remote;

/** Peer time learned only from an authenticated response bound to our fresh request.
 * All durations use the caller's monotonic clock; no system clock is changed. */
public final class RemoteClock {
    private long sampledAt=-1,peerMillis;
    public synchronized boolean ready(long elapsedMillis){return sampledAt>=0&&elapsedMillis>=sampledAt&&elapsedMillis-sampledAt<=90000;}
    public synchronized void observe(long serverSeconds,long sentElapsedMillis,long receivedElapsedMillis){
        long rtt=receivedElapsedMillis-sentElapsedMillis;
        if(sentElapsedMillis<0||rtt<0||rtt>10000)throw new IllegalArgumentException("网络延迟过大，无法安全校准连接时间，请稍后重新连接");
        if(serverSeconds<=0||serverSeconds>Long.MAX_VALUE/1000-100)throw new IllegalArgumentException("电脑返回的连接时间无效");
        if(receivedElapsedMillis<sampledAt)return;
        peerMillis=serverSeconds*1000+rtt/2;sampledAt=receivedElapsedMillis;
    }
    public synchronized long seconds(long elapsedMillis,long fallbackSeconds){
        return ready(elapsedMillis)?(peerMillis+elapsedMillis-sampledAt)/1000:fallbackSeconds;
    }
    public static boolean retryStatus(int code,String action,boolean authenticatedHint,boolean alreadyRetried){
        return code==408&&"status".equals(action)&&authenticatedHint&&!alreadyRetried;
    }
}
