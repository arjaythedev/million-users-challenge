// Independent black-box checks; run after the official suite. Requires Node >=18.
// node extended-regression.mjs http://127.0.0.1:3000 http://127.0.0.1:3001 [--idle]
import http from 'node:http';
import {createHmac} from 'node:crypto';
import {gunzipSync} from 'node:zlib';
import assert from 'node:assert/strict';
const urls = process.argv.slice(2).filter(x => !x.startsWith('--'));
if (!urls.length) urls.push('http://127.0.0.1:3000', 'http://127.0.0.1:3001');
const agents = urls.map(() => new http.Agent({keepAlive:true,maxSockets:32}));
const secret = process.env.JWT_SECRET || 'million-users-challenge';
let checks = 0;
function token(payload, header = {alg:'HS256',typ:'JWT'}, key = secret) {
  const b64 = x => Buffer.from(JSON.stringify(x)).toString('base64url');
  const msg = b64(header) + '.' + b64(payload);
  return msg + '.' + createHmac('sha256',key).update(msg).digest('base64url');
}
const claims = () => ({sub:'1',username:'golden_ember_1',exp:Math.floor(Date.now()/1000)+3600});
function request(port, method, path, {auth,body,encoding,raw,agent} = {}) {
  return new Promise((resolve,reject) => {
    const headers = {};
    if (auth !== undefined) headers.Authorization = auth;
    if (encoding !== undefined) headers['Accept-Encoding'] = encoding;
    const data = raw !== undefined ? raw : body !== undefined ? JSON.stringify(body) : undefined;
    if (data !== undefined) {
      headers['Content-Type'] = 'application/json';
      headers['Content-Length'] = Buffer.byteLength(data);
    }
    const req = http.request(new URL(path,urls[port]),{method,headers,agent:agent??agents[port]},res => {
      const parts=[];
      res.on('data',b => parts.push(b)); res.on('error',reject);
      res.on('end',() => {
        try {
          req.setTimeout(0);
          let bytes=Buffer.concat(parts);
          if(res.headers['content-encoding']==='gzip') bytes=gunzipSync(bytes);
          const text=bytes.toString('utf8');
          resolve({status:res.statusCode,headers:res.headers,text,data:JSON.parse(text),socket:req.socket});
        } catch(e){reject(e);}
      });
    });
    req.setTimeout(15000,() => req.destroy(new Error('request timeout')));
    req.on('error',reject); req.end(data);
  });
}
function expect(r,status,data) {
  assert.equal(r.status,status,r.text);
  assert.match(r.headers['content-type']||'',/^application\/json/);
  if(data!==undefined) assert.deepEqual(r.data,data);
  checks++;
}
function error(r,status,message){expect(r,status,{error:message});}
function postOrder(p){assert.deepEqual(Object.keys(p),['id','body','created_at','author','like_count']);}
try {
  for(let port=0;port<urls.length;port++){
    const health=await request(port,'GET','/health'); expect(health,200);
    assert.match(health.text,/^\{"status":"ok","db":"ok","uptime_s":\d+\}\n?$/);
    error(await request(port,'GET','/nonexistent'),404,'not found');
    const feed=await request(port,'GET','/feed',{encoding:'gzip'}); expect(feed,200);
    assert.equal(feed.data.posts.length,20); feed.data.posts.forEach(postOrder);
    for(const encoding of ['gzip;q=0','br, gzip;q=0','*;q=1, gzip;q=0']){
      const r=await request(port,'GET','/feed',{encoding}); expect(r,200);
      assert.notEqual(r.headers['content-encoding'],'gzip','gzip forbidden by '+encoding);
    }
    const valid='Bearer '+token(claims());
    for(const payload of [
      {...claims(),sub:1},{...claims(),sub:'0'},{...claims(),sub:'-1'},
      {...claims(),sub:'1.5'},{...claims(),username:17},{...claims(),username:null},
      {...claims(),sub:undefined},
    ]) error(await request(port,'POST','/posts',{auth:'Bearer '+token(payload),body:{body:'x'}}),401,'invalid token payload');
    for(const payload of [{...claims(),exp:undefined},{...claims(),exp:0},{...claims(),exp:'9999999999'}])
      error(await request(port,'POST','/posts',{auth:'Bearer '+token(payload),body:{body:'x'}}),401,'invalid or expired token');
    for(const bad of [token(claims(),{alg:'HS384'}),token(claims(),{alg:'none'}),token(claims(),undefined,'wrong-secret'),token(claims())+'.extra'])
      error(await request(port,'POST','/posts',{auth:'Bearer '+bad,body:{body:'x'}}),401,'invalid or expired token');
    error(await request(port,'POST','/posts/abc/like'),401,'missing bearer token');
    error(await request(port,'POST','/posts/abc/like',{auth:valid}),400,'invalid post id');
    for(const raw of ['{','{"body":',''])
      error(await request(port,'POST','/posts',{auth:valid,raw}),400,'malformed JSON body');
    for(const body of [null,[],123,{body:null},{body:[]},{body:false}])
      error(await request(port,'POST','/posts',{auth:valid,body}),400,'body is required');
    for(const text of ['😀'.repeat(500),'é'.repeat(500),'a\n"\\\t<> café ✓']){
      const r=await request(port,'POST','/posts',{auth:valid,body:{body:' \t'+text+'\r\n '}});
      expect(r,201); postOrder(r.data.post); assert.equal(r.data.post.body,text);
      assert.match(r.data.post.created_at,/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$/);
      const read=await request((port+1)%urls.length,'GET','/posts/'+r.data.post.id);
      expect(read,200,r.data);
    }
    error(await request(port,'POST','/posts',{auth:valid,body:{body:'😀'.repeat(501)}}),400,'body must be at most 500 characters');
    const alias='Bearer '+token({...claims(),username:'token-only-名'});
    const made=await request(port,'POST','/posts',{auth:alias,body:{body:'concurrent-like-regression'}});
    expect(made,201); assert.equal(made.data.post.author,'token-only-名');
    const id=made.data.post.id;
    const likes=await Promise.all(Array.from({length:20},(_,i)=>request(i%urls.length,'POST','/posts/'+id+'/like',{auth:valid})));
    assert.equal(likes.filter(r=>r.status===201).length,1);
    likes.forEach(r=>expect(r,r.data.already_liked?200:201,{liked:true,already_liked:r.data.already_liked,post_id:id}));
    const read=await request(port,'GET','/posts/'+id); expect(read,200);
    assert.equal(read.data.post.like_count,1); assert.equal(read.data.post.author,'golden_ember_1');
    const after=await request(port,'GET','/feed'); expect(after,200);
    assert.equal(after.data.posts[0].id,id); assert.equal(after.data.posts[0].like_count,1);
    console.log('PASS '+urls[port]+': routing, auth, gzip, Unicode, concurrent writes, freshness');
  }
  if(process.argv.includes('--idle')){
    const agent=new http.Agent({keepAlive:true,maxSockets:1,maxFreeSockets:1,timeout:0});
    try{
      const first=await request(0,'GET','/health',{agent}); expect(first,200);
      console.log('Checking same idle socket remains open for 76 seconds...');
      await new Promise(resolve=>setTimeout(resolve,76000));
      const second=await request(0,'GET','/health',{agent}); expect(second,200);
      assert.equal(second.socket,first.socket,'socket was closed/reconnected before 75-second minimum');
      console.log('PASS 76-second idle socket reuse');
    }finally{agent.destroy();}
  }
  console.log('PASS '+checks+' HTTP checks plus response invariants');
}finally{agents.forEach(agent=>agent.destroy());}
