import http from 'node:http';
import { timingSafeEqual } from 'node:crypto';
import { pathToFileURL } from 'node:url';

// Private, single-owner relay. It sees only AES-GCM envelopes, never pairing keys.
// Nothing is persisted: offline requests expire rather than executing on reconnect.
export function createRelay({ key, holdMs=20000, ttlMs=12000, maxChannels=64 }={}) {
  if(!/^[0-9a-fA-F]{64}$/.test(key??''))throw new Error('Set GUANYUAN_RELAY_KEY to a random 32-byte hex key.');
  const expected=Buffer.from(`Bearer ${key}`);
  const channels=new Map();
  const reply=(res,status,value)=>{
    if(res.destroyed||res.writableEnded)return;
    res.writeHead(status,{'content-type':'application/json; charset=utf-8','cache-control':'no-store','x-content-type-options':'nosniff'});
    res.end(JSON.stringify(value));
  };
  const fail=(res,status,error)=>reply(res,status,{ok:false,error});
  const id=s=>typeof s==='string'&&/^[0-9a-f]{32}$/.test(s);
  const validEnvelope=e=>e&&id(e.clientId)&&id(e.requestId)&&typeof e.iv==='string'&&/^[A-Za-z0-9+/]{16}$/.test(e.iv)&&typeof e.data==='string'&&e.data.length>=24&&e.data.length<=8192&&/^[A-Za-z0-9+/]+={0,2}$/.test(e.data);
  function channel(pc) {
    let c=channels.get(pc);
    if(!c){if(channels.size>=maxChannels)return null;c={poll:null,last:0,pending:new Map(),queue:[]};channels.set(pc,c);}
    return c;
  }
  function drain(c){
    while(c.poll&&c.queue.length){
      const p=c.pending.get(c.queue.shift());if(!p)continue;
      const poll=c.poll;c.poll=null;c.last=Date.now();clearTimeout(poll.timer);reply(poll.res,200,{envelope:p.envelope});break;
    }
  }
  const server=http.createServer(async(req,res)=>{
    const supplied=Buffer.from(req.headers.authorization??'');
    if(supplied.length!==expected.length||!timingSafeEqual(supplied,expected)){fail(res,401,'中转访问密钥不正确');req.resume();return;}
    if(req.method==='GET'&&req.url==='/health'){reply(res,200,{ok:true,service:'GuanYuanRelay',version:1});return;}
    if(req.method!=='POST'||!['/v1/poll','/v1/dispatch','/v1/reply'].includes(req.url)){fail(res,404,'接口不存在');req.resume();return;}
    if(!/^application\/json(?:;|$)/i.test(req.headers['content-type']??'')){fail(res,415,'需要 JSON 请求');req.resume();return;}
    let data='';try{
      for await(const bytes of req){if(Buffer.byteLength(data)+bytes.length>12288){fail(res,413,'请求过大');req.destroy();return;}data+=bytes.toString('utf8');}
      data=JSON.parse(data);
    }catch{fail(res,400,'请求内容无效');return;}
    if(!id(data?.id)){fail(res,400,'电脑标识无效');return;}
    const now=Date.now();
    if(req.url==='/v1/poll'){
      const c=channel(data.id);if(!c){fail(res,429,'连接数量已满');return;}
      if(c.poll){fail(res,409,'这台电脑已有中转连接');return;}
      const first=c.last===0;c.last=now;
      // A quick first acknowledgement lets the desktop report connected promptly.
      if(first){reply(res,200,{envelope:null});return;}
      const poll={res,timer:null};c.poll=poll;
      poll.timer=setTimeout(()=>{if(c.poll===poll){c.poll=null;c.last=Date.now();}reply(res,200,{envelope:null});},holdMs);
      res.on('close',()=>{if(c.poll===poll)c.poll=null;clearTimeout(poll.timer);});drain(c);return;
    }
    if(!validEnvelope(data.envelope)){fail(res,400,'加密消息格式无效');return;}
    const c=channels.get(data.id);
    if(req.url==='/v1/dispatch'){
      if(!c||(!c.poll&&now-c.last>5000)){fail(res,503,'电脑不在线，请确认电脑已开机并连接中转');return;}
      // Refresh liveness while a long poll is actually open.
      if(c.poll)c.last=now;
      if(c.pending.size>=8){fail(res,429,'电脑正在处理其他请求，请稍后再试');return;}
      const rid=data.envelope.requestId;
      if(c.pending.has(rid)){fail(res,409,'重复请求已拒绝');return;}
      const p={res,envelope:data.envelope,timer:null};c.pending.set(rid,p);c.queue.push(rid);
      const remove=()=>{if(c.pending.get(rid)===p)c.pending.delete(rid);c.queue=c.queue.filter(x=>x!==rid);clearTimeout(p.timer);};
      p.timer=setTimeout(()=>{remove();fail(res,504,'未收到电脑回执，请先检查状态，勿重复提交关机');},ttlMs);
      res.on('close',remove);drain(c);return;
    }
    const p=c?.pending.get(data.envelope.requestId);
    if(!p||p.envelope.clientId!==data.envelope.clientId){fail(res,410,'请求已过期');return;}
    c.pending.delete(data.envelope.requestId);clearTimeout(p.timer);reply(p.res,200,{envelope:data.envelope});reply(res,200,{ok:true});
  });
  server.requestTimeout=10000;server.headersTimeout=10000;server.maxHeadersCount=24;server.maxConnections=128;
  const sweep=setInterval(()=>{for(const [pc,c] of channels)if(!c.poll&&!c.pending.size&&Date.now()-c.last>60000)channels.delete(pc);},30000);sweep.unref();
  server.on('close',()=>{clearInterval(sweep);for(const c of channels.values()){if(c.poll){clearTimeout(c.poll.timer);fail(c.poll.res,503,'中转重启中');}for(const p of c.pending.values()){clearTimeout(p.timer);fail(p.res,503,'中转重启中');}}channels.clear();});
  return server;
}
if(process.argv[1]&&import.meta.url===pathToFileURL(process.argv[1]).href){
  const server=createRelay({key:process.env.GUANYUAN_RELAY_KEY});
  server.listen(Number(process.env.PORT??18779),process.env.BIND??'127.0.0.1',()=>{process.stdout.write('GuanYuan relay started. HTTPS reverse proxy required for internet use.\n');});
  const stop=()=>{server.close();server.closeAllConnections();};process.on('SIGTERM',stop);process.on('SIGINT',stop);
}
