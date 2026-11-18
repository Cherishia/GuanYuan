import http from 'node:http';
import https from 'node:https';
import {spawn} from 'node:child_process';
import {randomBytes} from 'node:crypto';
import {mkdir,readFile,writeFile,access} from 'node:fs/promises';
import {resolve,join,dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
const [exe,jdk,folderArg]=process.argv.slice(2);
if(!exe||!jdk||!folderArg)throw new Error('Provide exe, JDK and work directory.');
const folder=resolve(folderArg);await mkdir(folder,{recursive:true});const source=dirname(fileURLToPath(import.meta.url));
const pause=ms=>new Promise(r=>setTimeout(r,ms));
function run(file,args){return new Promise((ok,no)=>{const p=spawn(file,args,{windowsHide:true,stdio:'inherit'});p.on('error',no);p.on('exit',c=>c===0?ok():no(new Error('Test process exited '+c)));});}
const cert=join(folder,randomBytes(6).toString('hex')+'.p12'),password=randomBytes(12).toString('hex');
await run(join(jdk,'bin/keytool.exe'),['-genkeypair','-alias','test','-keystore',cert,'-storetype','PKCS12','-storepass',password,'-keypass',password,'-keyalg','RSA','-keysize','2048','-validity','2','-dname','CN=localhost','-ext','SAN=ip:127.0.0.1,dns:localhost','-noprompt']);
const topics=new Map();let publications=0;
async function handler(req,res){
 const match=/^\/([A-Za-z0-9_-]{1,64})(\/raw)?$/.exec(req.url??'');if(!match){res.writeHead(404).end();return;}
 const topic=match[1];let subs=topics.get(topic);if(!subs){subs=new Set();topics.set(topic,subs);}
 if(req.method==='GET'&&match[2]){res.writeHead(200,{'content-type':'text/plain','cache-control':'no-store'});res.write('\n');subs.add(res);res.on('close',()=>subs.delete(res));return;}
 if(req.method!=='POST'||req.headers.cache!=='no'||req.headers.firebase!=='no'){res.writeHead(400).end();return;}
 let body='';for await(const b of req){body+=b.toString();if(Buffer.byteLength(body)>4096){res.writeHead(413).end();return;}}
 if(!/^GuanYuan connectivity [a-f0-9]{32}$/.test(body))JSON.parse(body);publications++;for(const sub of subs)sub.write(body+'\n');
 res.writeHead(200,{'content-type':'application/json'}).end(JSON.stringify({event:'message',message:body}));
}
const plain=http.createServer(handler),tls=https.createServer({pfx:await readFile(cert),passphrase:password},handler);
await Promise.all([new Promise(r=>plain.listen(0,'127.0.0.1',r)),new Promise(r=>tls.listen(0,'127.0.0.1',r))]);
const pcUrl='http://127.0.0.1:'+plain.address().port,phoneUrl='https://127.0.0.1:'+tls.address().port,key=randomBytes(32).toString('hex');
const fixture=join(folder,randomBytes(6).toString('hex')+'.json');
const pc=spawn(resolve(exe),['--integration-server',fixture,'--test-relay-url',pcUrl,'--test-relay-key',key,'--test-relay-mode','ntfy'],{windowsHide:true,stdio:'ignore'});
try{
 let ready=false;for(let i=0;i<100;i++){try{await access(fixture);ready=true;break;}catch{}await pause(100);}if(!ready)throw new Error('Mock PC did not start');
 const classes=join(folder,'classes');await mkdir(classes,{recursive:true});
 await run(join(jdk,'bin/javac.exe'),['-J-Dfile.encoding=UTF-8','-encoding','UTF-8','-d',classes,join(source,'../Android/src/local/palmpower/remote/Protocol.java'),join(source,'../Android/src/local/palmpower/remote/NtfyTransport.java'),join(source,'../Android/src/local/palmpower/remote/RemoteClock.java'),join(source,'ProtocolIntegration.java')]);
 await run(join(jdk,'bin/java.exe'),['-Dfile.encoding=UTF-8','-Djavax.net.ssl.trustStore='+cert,'-Djavax.net.ssl.trustStorePassword='+password,'-Djavax.net.ssl.trustStoreType=PKCS12','-cp',classes,'ProtocolIntegration',fixture,phoneUrl,key,'ntfy',pcUrl]);
 await run(join(jdk,'bin/javac.exe'),['-encoding','UTF-8','-cp',classes,'-d',classes,join(source,'NtfyDiagnosticIntegration.java')]);
 await run(join(jdk,'bin/java.exe'),['-Dfile.encoding=UTF-8','-Djavax.net.ssl.trustStore='+cert,'-Djavax.net.ssl.trustStorePassword='+password,'-Djavax.net.ssl.trustStoreType=PKCS12','-cp',classes,'NtfyDiagnosticIntegration',phoneUrl]);
 if(publications<20)throw new Error('Expected request and reply publications');console.log('PASS: ntfy raw stream; Cache:no and Firebase:no applied to every message');
}finally{
 await writeFile(fixture+'.stop','');for(let i=0;i<70&&pc.exitCode===null;i++)await pause(100);if(pc.exitCode===null)pc.kill();
 for(const set of topics.values())for(const res of set)res.end();plain.closeAllConnections();tls.closeAllConnections();
 await Promise.all([new Promise(r=>plain.close(r)),new Promise(r=>tls.close(r))]);
}
