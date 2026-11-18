import {createRelay} from '../../Relay/server.mjs';
import {spawn} from 'node:child_process';
import {randomBytes} from 'node:crypto';
import {readFile,writeFile,mkdir,access} from 'node:fs/promises';
import {resolve,join,dirname} from 'node:path';
import {fileURLToPath} from 'node:url';

const [exe,jdk,folderArg]=process.argv.slice(2);
if(!exe||!jdk||!folderArg)throw new Error('Usage: node Run-RemoteIntegration.mjs <exe> <jdk-directory> <build-directory>');
const folder=resolve(folderArg);await mkdir(folder,{recursive:true});
const root=dirname(fileURLToPath(import.meta.url));const key=randomBytes(32).toString('hex');
const publicTest=process.env.GUANYUAN_PUBLIC_TEST==='1';
const relay=publicTest?null:createRelay({key});if(relay)await new Promise(r=>relay.listen(0,'127.0.0.1',r));const url=publicTest?'https://ntfy.sh':`http://127.0.0.1:${relay.address().port}`;
const fixture=join(folder,randomBytes(8).toString('hex')+'.json');
const pc=spawn(resolve(exe),['--integration-server',fixture,'--test-relay-url',url,'--test-relay-key',key,...publicTest?['--test-relay-mode','ntfy']:[]],{windowsHide:true,stdio:'ignore'});
const pause=ms=>new Promise(r=>setTimeout(r,ms));
function run(file,args){return new Promise((ok,no)=>{const p=spawn(file,args,{windowsHide:true,stdio:'inherit'});p.on('error',no);p.on('exit',c=>c===0?ok():no(new Error('Test tool exited '+c)));});}
try{
  let ready=false;for(let i=0;i<100;i++){try{await access(fixture);ready=true;break;}catch{}await pause(100);}
  if(!ready)throw new Error('Mock computer server did not start');
  const info=JSON.parse(await readFile(fixture,'utf8'));if(!info.info.simulated)throw new Error('Only simulated executors may be tested');
  const classes=join(folder,'classes');await mkdir(classes,{recursive:true});
  await run(join(jdk,'bin/javac.exe'),['-J-Dfile.encoding=UTF-8','-encoding','UTF-8','-d',classes,join(root,'../Android/src/local/palmpower/remote/Protocol.java'),join(root,'../Android/src/local/palmpower/remote/NtfyTransport.java'),join(root,'../Android/src/local/palmpower/remote/RemoteClock.java'),join(root,'ProtocolIntegration.java')]);
  if(publicTest)await pause(2500);
  await run(join(jdk,'bin/java.exe'),['-Dfile.encoding=UTF-8','-cp',classes,'ProtocolIntegration',fixture,url,key,...publicTest?['ntfy']:[]]);
}finally{
  await writeFile(fixture+'.stop','');for(let i=0;i<70&&pc.exitCode===null;i++)await pause(100);if(pc.exitCode===null)pc.kill();
  if(relay){relay.closeAllConnections();await new Promise(r=>relay.close(r));}
}
