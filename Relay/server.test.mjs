import {test} from 'node:test';
import assert from 'node:assert/strict';
import {createRelay} from './server.mjs';
const key='ab'.repeat(32),pc='11'.repeat(16),client='22'.repeat(16);
const envelope=(request='33'.repeat(16))=>({clientId:client,requestId:request,iv:Buffer.alloc(12).toString('base64'),data:Buffer.alloc(32).toString('base64')});
test('relay authentication, routing, expiry and bounded queues',async t=>{
  const s=createRelay({key,holdMs:300,ttlMs:180,maxChannels:1});await new Promise(r=>s.listen(0,'127.0.0.1',r));
  const base=`http://127.0.0.1:${s.address().port}`;
  async function send(path,data,secret=key){const response=await fetch(base+path,{method:data===undefined?'GET':'POST',headers:{Authorization:`Bearer ${secret}`,'Content-Type':'application/json'},body:data===undefined?undefined:JSON.stringify(data)});return {status:response.status,body:await response.json()};}
  try{
    await t.test('wrong credentials rejected',async()=>assert.equal((await send('/health',undefined,'ff'.repeat(32))).status,401));
    await t.test('authenticated health',async()=>assert.equal((await send('/health')).body.ok,true));
    await t.test('offline commands rejected',async()=>assert.equal((await send('/v1/dispatch',{id:pc,envelope:envelope()})).status,503));
    await t.test('invalid envelopes rejected',async()=>assert.equal((await send('/v1/dispatch',{id:pc,envelope:{}})).status,400));
    await t.test('initial poll acknowledged',async()=>assert.equal((await send('/v1/poll',{id:pc})).status,200));
    await t.test('channel count bounded',async()=>assert.equal((await send('/v1/poll',{id:'99'.repeat(16)})).status,429));
    await t.test('encrypted messages round trip unchanged',async()=>{
      const poll=send('/v1/poll',{id:pc});const request=envelope();const dispatch=send('/v1/dispatch',{id:pc,envelope:request});
      assert.deepEqual((await poll).body.envelope,request);
      const answer={...request,data:Buffer.alloc(48,7).toString('base64')};assert.equal((await send('/v1/reply',{id:pc,envelope:answer})).status,200);assert.deepEqual((await dispatch).body.envelope,answer);
    });
    await t.test('timeouts discard requests and reject late replies',async()=>{
      const poll=send('/v1/poll',{id:pc});const request=envelope('44'.repeat(16));const dispatch=send('/v1/dispatch',{id:pc,envelope:request});await poll;
      assert.equal((await dispatch).status,504);assert.equal((await send('/v1/reply',{id:pc,envelope:request})).status,410);
      assert.equal((await send('/v1/poll',{id:pc})).body.envelope,null);
    });
    await t.test('duplicate live request rejected',async()=>{
      const poll=send('/v1/poll',{id:pc});const request=envelope('55'.repeat(16));const dispatch=send('/v1/dispatch',{id:pc,envelope:request});await poll;
      assert.equal((await send('/v1/dispatch',{id:pc,envelope:request})).status,409);await send('/v1/reply',{id:pc,envelope:request});await dispatch;
    });
  }finally{s.closeAllConnections();await new Promise(r=>s.close(r));}
});
