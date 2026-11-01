// Same-host diagnostic only: this is NOT the challenge's user/capacity score.
// node tests/bench.mjs --target http://127.0.0.1:3000 --seconds 15 --connections 128 --workers 4
// Optional --workload mixed: 47% feed, 48% post reads, 4% likes, 1% creates.
import http from 'node:http';
import https from 'node:https';
import {Worker, isMainThread, parentPort, workerData} from 'node:worker_threads';
import {performance} from 'node:perf_hooks';
import {gunzipSync} from 'node:zlib';
import {createHmac} from 'node:crypto';

const HIST_SIZE = 2000;
const LOG_STEP = Math.log(1.01);
function bucket(milliseconds) {
  return Math.max(0, Math.min(HIST_SIZE - 1, Math.floor(Math.log(Math.max(1, milliseconds * 1000)) / LOG_STEP)));
}
function percentile(hist, fraction) {
  const total = hist.reduce((a, b) => a + b, 0);
  if (!total) return null;
  const target = Math.ceil(total * fraction);
  let sum = 0;
  for (let i = 0; i < hist.length; i++) {
    sum += hist[i];
    if (sum >= target) return Math.round(Math.exp((i + 1) * LOG_STEP) / 1000 * 1000) / 1000;
  }
}
function integer(value, name, min, max) {
  const n = Number(value);
  if (!Number.isInteger(n) || n < min || n > max) throw new Error(name + ' must be an integer in [' + min + ', ' + max + ']');
  return n;
}
function options() {
  const defaults = {target:'http://127.0.0.1:3000',seconds:'15',connections:'128',workers:'4',workload:'feed'};
  const args = process.argv.slice(2);
  for (let i = 0; i < args.length; i += 2) {
    const key = args[i]?.replace(/^--/, '');
    if (!(key in defaults) || !args[i+1]) throw new Error('Use --target URL[,URL] --seconds N --connections N --workers N --workload feed|mixed');
    defaults[key] = args[i+1];
  }
  const config = {
    targets:defaults.target.split(','),
    seconds:integer(defaults.seconds,'seconds',1,3600),
    connections:integer(defaults.connections,'connections',1,8192),
    workers:integer(defaults.workers,'workers',1,64),
    workload:defaults.workload,
  };
  if (!['feed','mixed'].includes(config.workload)) throw new Error('workload must be feed or mixed');
  for (const target of config.targets) if (!['http:','https:'].includes(new URL(target).protocol)) throw new Error('target must use http or https');
  config.workers = Math.min(config.workers, config.connections);
  return config;
}
function signToken() {
  const b64 = x => Buffer.from(JSON.stringify(x)).toString('base64url');
  const input = b64({alg:'HS256',typ:'JWT'}) + '.' + b64({sub:'1',username:'golden_ember_1',exp:Math.floor(Date.now()/1000)+7200});
  return input + '.' + createHmac('sha256', process.env.JWT_SECRET || 'million-users-challenge').update(input).digest('base64url');
}

if (isMainThread) {
  try {
    const config = options();
    const startAt = Date.now() + 1500;
    const children = [];
    const promises = Array.from({length:config.workers}, (_, index) => new Promise((resolve,reject) => {
      const connections = Math.floor(config.connections/config.workers) + (index < config.connections%config.workers ? 1 : 0);
      const worker = new Worker(new URL(import.meta.url), {workerData:{...config,index,connections,startAt}});
      children.push(worker);
      let result;
      worker.on('message', message => { result = message; });
      worker.on('error', reject);
      worker.on('exit', code => code === 0 && result ? resolve(result) : reject(new Error('worker '+index+' exited '+code+' without complete results')));
    }));
    const watchdog = setTimeout(() => {
      children.forEach(worker => worker.terminate());
    }, config.seconds*1000 + 35000);
    let results;
    try { results = await Promise.all(promises); }
    finally { clearTimeout(watchdog); await Promise.all(children.map(worker => worker.terminate())); }
    const histogram = Array(HIST_SIZE).fill(0);
    const failedHistogram = Array(HIST_SIZE).fill(0);
    const errors = {}, endpoints = {};
    let successful = 0, failed = 0, wireBytes = 0, decodedBytes = 0, gzipResponses = 0;
    for (const result of results) {
      successful += result.successful; failed += result.failed;
      wireBytes += result.wireBytes; decodedBytes += result.decodedBytes; gzipResponses += result.gzipResponses;
      result.histogram.forEach((n,i) => histogram[i] += n);
      result.failedHistogram.forEach((n,i) => failedHistogram[i] += n);
      for (const [key,value] of Object.entries(result.errors)) errors[key] = (errors[key]||0) + value;
      for (const [key,value] of Object.entries(result.endpoints)) endpoints[key] = (endpoints[key]||0) + value;
    }
    const elapsedSeconds = (Math.max(...results.map(r => r.finishedAt)) - startAt) / 1000;
    console.log(JSON.stringify({
      benchmark:'same-host closed-loop diagnostic; not an official million-user score',
      workload:config.workload === 'feed' ? 'GET /feed only' : 'deterministic 47% feed / 48% post reads / 4% likes / 1% creates',
      targets:config.targets, workers:config.workers, connections:config.connections,
      requested_seconds:config.seconds, actual_seconds:Number(elapsedSeconds.toFixed(3)),
      requests:successful+failed, successful, errors:failed,
      successful_requests_per_second:Number((successful/elapsedSeconds).toFixed(2)),
      error_rate:Number((failed/Math.max(1,successful+failed)).toFixed(6)),
      latency_ms:{p50:percentile(histogram,0.50),p95:percentile(histogram,0.95),p99:percentile(histogram,0.99)},
      failed_latency_ms:{p95:percentile(failedHistogram,0.95)},
      transferred_body_bytes:wireBytes, decoded_body_bytes:decodedBytes, gzip_responses:gzipResponses,
      wire_body_megabytes_per_second:Number((wireBytes/elapsedSeconds/1e6).toFixed(3)),
      errors_by_kind:errors, requests_by_endpoint:endpoints,
      notes:[
        'Accept-Encoding: gzip is sent on every request; gzip is decoded and JSON/status validated.',
        'Transferred bytes count response bodies as received, excluding HTTP/TCP headers.',
        'Successful latency includes response drain, gzip decoding, and JSON validation; histogram resolution is approximately 1%.',
        'Requests started before the deadline finish or time out before workers exit; actual elapsed time includes that drain.',
        'Mixed mode writes data and uses user 1; run against a disposable seeded database.',
        'The load generator shares host resources if targets are local; compare identical conditions and report this limitation.'
      ],
    }, null, 2));
    if (!successful || failed > 0) process.exitCode = 1;
  } catch (error) {
    console.error(error.stack || String(error));
    process.exitCode = 1;
  }
} else {
  const {connections,index,startAt,seconds,targets,workload} = workerData;
  const deadline = startAt + seconds*1000;
  const agents = targets.map(target => new (new URL(target).protocol === 'https:' ? https.Agent : http.Agent)({keepAlive:true,maxSockets:connections,maxFreeSockets:connections}));
  const histogram = Array(HIST_SIZE).fill(0), failedHistogram = Array(HIST_SIZE).fill(0);
  const errors = {}, endpoints = {};
  let successful=0, failed=0, wireBytes=0, decodedBytes=0, gzipResponses=0;
  const authorization = 'Bearer '+signToken();
  function request(client, sequence, latestId) {
    let path='/feed', method='GET', body, kind='GET /feed';
    if (workload === 'mixed') {
      const choice=(sequence+client*17+index*23)%100;
      if (choice>=47 && choice<95) { path='/posts/'+latestId; kind='GET /posts/:id'; }
      else if(choice>=95 && choice<99) { path='/posts/'+latestId+'/like'; method='POST'; kind='POST /posts/:id/like'; }
      else if(choice===99) { path='/posts'; method='POST'; kind='POST /posts'; body=JSON.stringify({body:'diagnostic worker '+index+' client '+client+' request '+sequence}); }
    }
    const targetIndex=(index+client)%targets.length;
    const url=new URL(path,targets[targetIndex]);
    const transport=url.protocol==='https:'?https:http;
    const headers={'Accept-Encoding':'gzip'};
    if(method==='POST') headers.Authorization=authorization;
    if(body!==undefined){headers['Content-Type']='application/json';headers['Content-Length']=Buffer.byteLength(body);}
    endpoints[kind]=(endpoints[kind]||0)+1;
    return new Promise((resolve,reject)=>{
      const req=transport.request(url,{method,headers,agent:agents[targetIndex]},res=>{
        const parts=[];
        res.on('data',chunk=>{wireBytes+=chunk.length;parts.push(chunk);});
        res.on('error',reject);
        res.on('end',()=>{
          req.setTimeout(0);
          try{
            if(res.statusCode!==200 && res.statusCode!==201) throw new Error('HTTP '+res.statusCode+' '+kind);
            let data=Buffer.concat(parts);
            if(res.headers['content-encoding']==='gzip'){data=gunzipSync(data);gzipResponses++;}
            else if(res.headers['content-encoding']) throw new Error('unsupported content-encoding');
            decodedBytes+=data.length;
            if(!String(res.headers['content-type']||'').startsWith('application/json')) throw new Error('non-JSON content-type');
            const json=JSON.parse(data);
            if(kind==='GET /feed') {
              if(res.statusCode!==200 || !Array.isArray(json.posts) || json.posts.length!==20) throw new Error('invalid feed response');
              latestId=json.posts[(client+sequence)%20]?.id;
              if(!Number.isSafeInteger(latestId)||latestId<=0) throw new Error('invalid feed post id');
            } else if(kind==='GET /posts/:id' || kind==='POST /posts') {
              if(!json.post || !Number.isSafeInteger(json.post.id) || typeof json.post.body!=='string' || res.statusCode!==(method==='POST'?201:200)) throw new Error('invalid post response');
            } else if(json.liked!==true || json.post_id!==latestId || typeof json.already_liked!=='boolean' || res.statusCode!==(json.already_liked?200:201)) throw new Error('invalid like response');
            resolve(latestId);
          }catch(error){reject(error);}
        });
      });
      req.setTimeout(10000,()=>req.destroy(new Error('request timeout')));
      req.on('error',reject);
      req.end(body);
    });
  }
  try {
    await new Promise(resolve=>setTimeout(resolve,Math.max(0,startAt-Date.now())));
    await Promise.all(Array.from({length:connections},async(_,client)=>{
      let sequence=0, latestId=1;
      while(Date.now()<deadline){
        const began=performance.now();
        try{
          latestId=await request(client,sequence++,latestId);
          histogram[bucket(performance.now()-began)]++;
          successful++;
        }catch(error){
          failedHistogram[bucket(performance.now()-began)]++;
          failed++;
          const key=error.code || error.message || 'unknown';
          errors[key]=(errors[key]||0)+1;
          // Prevent a dead endpoint from creating an unbounded hot error loop.
          await new Promise(resolve=>setTimeout(resolve,10));
        }
      }
    }));
    parentPort.postMessage({successful,failed,wireBytes,decodedBytes,gzipResponses,histogram,failedHistogram,errors,endpoints,finishedAt:Date.now()});
  } finally { agents.forEach(agent=>agent.destroy()); }
}
