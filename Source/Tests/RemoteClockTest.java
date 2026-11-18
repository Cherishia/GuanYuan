import local.palmpower.remote.RemoteClock;
public class RemoteClockTest {
    interface Attempt{void run();}
    static void check(boolean ok,String label){if(!ok)throw new AssertionError(label);System.out.println("PASS: "+label);}
    static boolean denied(Attempt run){try{run.run();return false;}catch(IllegalArgumentException expected){return true;}}
    public static void main(String[] args){
        RemoteClock c=new RemoteClock();check(c.seconds(1000,2000000000)==2000000000&&!c.ready(1000),"Uncalibrated status can bootstrap using local time");
        c.observe(1700000000,1000,1200);
        check(c.seconds(2200,2000000000)==1700000001,"Authenticated peer time corrects a phone clock years ahead");
        check(c.seconds(3200,1000000000)==1700000002,"Phone wall-clock changes do not affect calibrated request time");
        check(c.ready(91200)&&!c.ready(91201),"Old peer clock sample requires a fresh status before power");
        check(!c.ready(1199),"Monotonic clock moving backwards invalidates the sample");
        c.observe(1600000000,100,1100);check(c.seconds(3200,1)==1700000002,"An older concurrent response cannot replace a newer sample");
        check(denied(()->c.observe(1700000000,0,10001)),"Excessive round trip cannot establish a misleading clock sample");
        check(denied(()->c.observe(1700000000,1000,900)),"Negative timing sample rejected");
        check(denied(()->c.observe(Long.MAX_VALUE,1000,1200))&&denied(()->c.observe(0,1000,1200)),"Invalid or overflowing peer time rejected");
        check(RemoteClock.retryStatus(408,"status",true,false),"Authenticated expiry rejection permits one status retry");
        check(!RemoteClock.retryStatus(408,"status",true,true)&&!RemoteClock.retryStatus(408,"status",false,false),"Repeated or unauthenticated clock retry refused");
        for(String action:new String[]{"shutdown","sleep","hibernate","cancel"})check(!RemoteClock.retryStatus(408,action,true,false),"Clock correction never automatically retries "+action);
    }
}
