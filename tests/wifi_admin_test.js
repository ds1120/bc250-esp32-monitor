const fs=require('fs'),vm=require('vm'),assert=require('assert');
const code=fs.readFileSync('sdcard/bc250/app.js','utf8'),html=fs.readFileSync('sdcard/bc250/index.html','utf8');
assert(!html.includes('usb-test-card'));assert(!html.includes('usb-bt-state'));assert(!code.includes('/api/usb-'));assert(!code.includes('showUSBFeatures'));
const elements=new Map();const forbidden=new Set(['usb-bt-state','usb-test-result']);
const document={getElementById(id){assert(!forbidden.has(id));if(!elements.has(id))elements.set(id,{});return elements.get(id)}};
const state={agent_online:true,relay_output:'HIGH',metrics_poll_ms:5000,relay_policy_self_test_ok:true,esp_restart_allowed:true,esp_restart_supported:true,relay_restart_hold_supported:true,ota_keep_power_supported:true,ota_supported:true,uptime_seconds:60};
const context={document,window:{},performance:{now:()=>0},AbortController,fetch:async()=>({ok:true,json:async()=>state}),setTimeout:()=>1,clearTimeout:()=>{},showMetrics:()=>{},updateOverview:()=>{},relayBusy:false};vm.createContext(context);
vm.runInContext(code.split('\n').find(line=>line.startsWith('async function refresh()')),context);
context.refresh().then(()=>{assert.equal(elements.get('state').textContent,'BC250 연결됨');assert.equal(elements.get('off').disabled,false);assert.equal(context.window.metricsRefreshBusy,false);console.log('PASS: Wi-Fi-only admin refresh, power controls and removed USB elements');}).catch(error=>{console.error(error);process.exitCode=1});
