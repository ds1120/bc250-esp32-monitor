
const popupQueue=[];let popupTimer=0,popupCurrent=null;
function notifyStatus(title,message){if(!message)return;if(popupCurrent&&popupCurrent.title===title&&popupCurrent.message===message)return;if(popupQueue.some(p=>p.title===title&&p.message===message))return;popupQueue.push({title,message});if(popupQueue.length>12)popupQueue.shift();if(!popupCurrent)showNextStatusPopup();}
function showNextStatusPopup(){clearTimeout(popupTimer);popupCurrent=popupQueue.shift()||null;const box=document.getElementById('status-popup');box.hidden=!popupCurrent;if(!popupCurrent)return;document.getElementById('popup-title').textContent=popupCurrent.title;document.getElementById('popup-message').textContent=popupCurrent.message;popupTimer=setTimeout(dismissStatusPopup,3000);}
function dismissStatusPopup(){showNextStatusPopup();}
document.addEventListener('keydown',e=>{if(e.key==='Escape'&&popupCurrent){e.preventDefault();dismissStatusPopup();}});
const popupTargets={'result':'운영체제','relay-result':'파워 전원','pad-result':'패드 설정','update-result':'펌웨어 업데이트','sd-result':'SD카드 작업','esp-restart-result':'ESP32 재부팅','sd-state':'SD카드 상태','state':'BC250 연결','supply':'파워 상태','gamepad-state':'패드 부팅 상태'};
const previousPopupStates=new Map();
for(const [id,title] of Object.entries(popupTargets)){
const el=document.getElementById(id);if(!el)continue;
const isResult=id.endsWith('result');
new MutationObserver(()=>{const value=el.textContent.trim();if(!value)return;const previous=previousPopupStates.get(id);previousPopupStates.set(id,value);
if(isResult){notifyStatus(title,value);return;}
if(previous===value)return;
if(previous===undefined&&id!=='sd-state')return;
notifyStatus(title,value);
}).observe(el,{childList:true,characterData:true,subtree:true});}

function applyTheme(value){const theme=['default','dark','light','neon'].includes(value)?value:'default';document.documentElement.dataset.theme=theme;document.getElementById('theme-choice').value=theme;try{localStorage.setItem('bc250-theme',theme);}catch(e){}}
try{applyTheme(localStorage.getItem('bc250-theme')||'default');}catch(e){applyTheme('default');}

let relayBusy=false,padLoaded=false,padSaving=false;
async function refreshGamepads(){try{
const r=await fetch('/api/gamepads',{cache:'no-store'});if(!r.ok)return;const g=await r.json();
document.getElementById('gamepad-power').textContent=g.power_result?'마지막 패드 전원 요청: '+g.power_result+' · 시도 '+g.power_attempts+'회'+(g.power_error?' · '+g.power_error:''):'전원 요청 결과 기록은 펌웨어 5.4부터 지원합니다.';
document.getElementById('gamepad-state').textContent=g.restart_after_shutdown?'주소 적용 대기 · 정상 종료 후 ESP32 재시작 필요':g.disabled?'패드 부팅 사용 안 함':g.error?'수신 상태 확인 필요: '+g.error:g.armed?'대기 준비 · 패드 전원/Home 버튼으로 켜기':g.listening?'패드 재연결 신호가 끝나기를 기다리는 중':'서버 동작 중 · 종료 후 켜기 대기';
document.getElementById('gamepad-list').textContent=(g.controllers||[]).map(p=>p.name+' · '+p.address).join(' / ')||'리눅스에 등록된 지원 패드가 없습니다.';
document.getElementById('gamepad-mode').textContent=g.manual?'수동 주소 사용 · 자동 동기화 중지':'리눅스 등록 정보 자동 동기화';
if(!padLoaded){document.getElementById('pad-host').value=g.host_mac||'';const box=document.getElementById('pad-addresses');box.replaceChildren();for(const p of g.controllers||[]){const label=document.createElement('label');label.textContent=p.name+' 패드 주소';const input=document.createElement('input');input.type='text';input.maxLength=17;input.value=p.address;input.className='pad-mac';label.append(input);box.append(label);}padLoaded=true;}
}catch(e){}}
async function saveGamepads(manual){
if(padSaving)return;const result=document.getElementById('pad-result'),host_mac=document.getElementById('pad-host').value.trim().toUpperCase(),addresses=Array.from(document.querySelectorAll('.pad-mac'),e=>e.value.trim().toUpperCase());
if(manual&&![host_mac,...addresses].every(a=>/^[0-9A-F]{2}(:[0-9A-F]{2}){5}$/.test(a)&&a!=='00:00:00:00:00:00'&&(parseInt(a.slice(0,2),16)&1)===0)){result.textContent='유효한 Bluetooth 주소를 입력하세요.';return;}
padSaving=true;try{const r=await fetch('/api/gamepads/config',{method:'POST',headers:{'Content-Type':'application/json','X-BC250-UI':'1'},body:JSON.stringify({manual,host_mac,addresses})});if(!r.ok)throw Error();const d=await r.json();result.textContent=d.restart_after_shutdown?'저장 완료 · 정상 종료 후 ESP32 재시작 시 적용됩니다.':manual?'주소를 저장했습니다.':'자동 동기화를 켰습니다. 서버 연결 시 등록 정보가 반영됩니다.';padLoaded=false;await refreshGamepads();}catch(e){result.textContent='저장하지 못했습니다. 주소와 연결 상태를 확인하세요.';}finally{padSaving=false;}}

const metricCache={},metricTimes={};let metricsLastReceived=null,metricsOnline=false,metricsTransport='wifi',metricsInterval=5000;
function renderMetrics(){const now=performance.now(),m={};for(const [key,value] of Object.entries(metricCache))if(now-metricTimes[key]<10000)m[key]=value;const value=(n,d=0)=>typeof n==='number'&&Number.isFinite(n)?n.toFixed(d):'--';for(const k of ['cpu','gpu']){document.getElementById(k).textContent=value(m[k+'_percent'])+' %';document.getElementById(k+'-temp').textContent=value(m[k+'_temp_c'],1)+' °C';}for(const k of ['ram','vram'])document.getElementById(k).textContent=value(m[k+'_used_gib'],1)+' / '+value(m[k+'_total_gib'],1);const age=metricsLastReceived===null?Infinity:now-metricsLastReceived;document.getElementById('metrics-state').textContent=age>=10000?'10초 이상 데이터 미수신 · 표시 대기':!metricsOnline||age>=metricsInterval+1000?'수신 지연 · 마지막 정상값 '+Math.floor(age/1000)+'초 전':Math.round(metricsInterval/1000)+'초마다 '+(metricsTransport==='usb'?'USB':'Wi-Fi')+' 갱신 · 미지원 센서는 --';}
function showMetrics(s){const now=performance.now(),modern='metrics_available' in s,incoming=modern?s.metrics_available:s.agent_online;metricsOnline=Boolean(s.agent_online);if(s.metrics_poll_ms)metricsInterval=s.metrics_poll_ms;if(s.metrics_transport)metricsTransport=s.metrics_transport;if(s.relay_output==='LOW'||modern&&!s.metrics_available){for(const k of Object.keys(metricCache)){delete metricCache[k];delete metricTimes[k];}metricsLastReceived=null;}else if(incoming){const at=now-(modern?(s.metrics_age_seconds||0)*1000:0);let valid=false;for(const [key,v] of Object.entries(s.metrics||{})){if(typeof v==='number'&&Number.isFinite(v)){metricCache[key]=v;metricTimes[key]=at;valid=true;}else if(modern){delete metricCache[key];delete metricTimes[key];}}if(valid)metricsLastReceived=at;}renderMetrics();}
setInterval(renderMetrics,1000);
async function setUSBBluetooth(enabled){return setUSBFeature('bluetooth',enabled);}
async function saveMonitorInterval(){const out=document.getElementById('monitor-settings-result');try{const poll_ms=Number(document.getElementById('monitor-interval').value);let r=await fetch('/api/monitor/settings',{method:'POST',headers:{'Content-Type':'application/json','X-BC250-UI':'1'},body:JSON.stringify({poll_ms})});if(r.status===404){const current=await fetch('/api/sd/settings',{cache:'no-store'});if(!current.ok)throw Error('현재 설정을 읽지 못했습니다.');const settings=await current.json();settings.metrics_poll_ms=poll_ms;const form=new FormData();form.append('file',new Blob([JSON.stringify(settings)],{type:'application/json'}),'settings.json');r=await fetch('/api/sd/file?name=settings.json',{method:'POST',headers:{'X-BC250-UI':'1'},body:form});}if(!r.ok)throw Error('저장하지 못했습니다. SD카드 상태를 확인하거나 펌웨어 5.5를 설치하세요.');metricsInterval=poll_ms;out.textContent='갱신 주기 '+poll_ms/1000+'초로 저장했습니다. 미수신 표시 기준은 10초입니다.';sdLoaded=false;}catch(e){out.textContent=e.message;}}
async function refresh(){if(window.metricsRefreshBusy)return;window.metricsRefreshBusy=true;window.metricsRefreshAt=performance.now();const c=new AbortController(),timer=setTimeout(()=>c.abort(),4000);try{let r=await fetch('/api/status',{cache:'no-store',signal:c.signal});if(!r.ok)throw Error();let s=await r.json();showMetrics(s);updateOverview(s);showUSBFeatures(s);if(!window.monitorSettingsLoaded){document.getElementById('monitor-interval').value=String(s.metrics_poll_ms||5000);window.monitorSettingsLoaded=true;}const ub=s.usb_bluetooth;document.getElementById('usb-bt-state').textContent=!ub?'펌웨어 5.5 설치 후 지원':!ub.enabled?'수신기 사용 안 함':ub.error?'확인 필요: '+ub.error:ub.active?'USB 블루투스 동작 · PC→ESP '+ub.host_packets+' / ESP→PC '+ub.controller_packets+' · 유실 '+ub.dropped_packets:'PC 종료 상태 · 패드 부팅 감지 대기';document.getElementById('esp-restart').disabled=!s.esp_restart_allowed;document.getElementById('esp-restart-note').textContent=!s.esp_restart_supported?'펌웨어 5.2 설치 후 사용할 수 있습니다.':s.esp_restart_allowed?(s.relay_restart_hold_supported?'현재 릴레이 출력을 유지하며 ESP32를 재시작합니다.':'ESP32만 재부팅합니다. Wi-Fi와 저장된 설정은 유지됩니다.'):'PC 정상 종료 후 전원 공급이 꺼지면 사용할 수 있습니다.';let on=s.relay_output==='HIGH';document.getElementById('supply').textContent=on?'전원 공급 요청 ON · 유지 중':'전원 공급 요청 OFF';document.getElementById('supply-detail').textContent=s.relay_cooldown_ms?'다시 켜기까지 '+Math.ceil(s.relay_cooldown_ms/1000)+'초 대기':'실제 파워와 BC250의 동작 여부는 별도로 확인하세요.';document.getElementById('on').disabled=relayBusy||on||s.relay_cooldown_ms>0||!s.relay_policy_self_test_ok;document.getElementById('cut').disabled=relayBusy||!on;window.keepRelayPowerOTA=Boolean(s.ota_keep_power_supported);document.getElementById('update').disabled=(on&&!s.ota_keep_power_supported)||s.ota_in_progress||!s.ota_supported;document.getElementById('state').textContent=s.agent_online?'BC250 연결됨':'BC250 응답 없음';document.getElementById('detail').textContent=s.agent_online?'켜진 시간 '+Math.floor(s.uptime_seconds/60)+'분 · '+(s.target||'연결됨'):'전원·네트워크·제어 서비스 연결을 확인해 주세요.';document.getElementById('off').disabled=document.getElementById('restart').disabled=!s.agent_online;}catch(e){showMetrics({agent_online:false});updateOverview({agent_online:false});document.getElementById('state').textContent='화면 연결 끊김';for(let id of ['on','cut','off','restart','update','esp-restart','display-off','display-on'])document.getElementById(id).disabled=true;}finally{clearTimeout(timer);window.metricsRefreshBusy=false;}}
async function setDisplay(awake){const result=document.getElementById('display-result');try{const r=await fetch('/api/display',{method:'POST',headers:{'Content-Type':'application/json','X-BC250-UI':'1'},body:JSON.stringify({awake})});if(!r.ok)throw Error();result.textContent=awake?'화면을 켰습니다.':'화면을 껐습니다.';}catch(e){result.textContent='화면 제어 실패 · 연결과 펌웨어 버전을 확인하세요.';}refresh();}
async function power(action){if(!confirm(action==='shutdown'?'BC250을 정상 종료할까요? 종료 후 파워 전원도 자동 차단됩니다.':'BC250을 재부팅할까요?'))return;document.getElementById('off').disabled=document.getElementById('restart').disabled=true;try{let r=await fetch('/api/power',{method:'POST',headers:{'Content-Type':'application/json','X-BC250-UI':'1'},body:JSON.stringify({action,confirm:true})});document.getElementById('result').textContent=r.ok?'요청을 접수했습니다.':'지금은 요청할 수 없습니다.';}catch(e){document.getElementById('result').textContent='연결을 확인해 주세요.';}refresh();}
async function relayAction(action){if(relayBusy)return;let message=action==='on'?'파워 전원을 켜고 계속 유지할까요?':'운영체제 종료 없이 전기를 즉시 끊습니다. 저장하지 않은 데이터가 손실될 수 있습니다. 강제 차단할까요?';if(!confirm(message))return;relayBusy=true;document.getElementById('on').disabled=document.getElementById('cut').disabled=true;try{let r=await fetch('/api/relay',{method:'POST',headers:{'Content-Type':'application/json','X-BC250-UI':'1'},body:JSON.stringify({action,confirm:true,force:action==='off'})});document.getElementById('relay-result').textContent=r.ok?(action==='on'?'켜기 요청을 유지합니다.':'전원 공급 요청을 OFF로 바꿨습니다.'):'요청할 수 없습니다. 잠시 후 상태를 확인하세요.';}catch(e){document.getElementById('relay-result').textContent='응답을 확인하지 못했습니다. 표시 상태와 실제 장치를 확인하세요.';}relayBusy=false;refresh();}

function firmwareHash(buffer){
const K=[0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2];
const H=[0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19],src=new Uint8Array(buffer),data=new Uint8Array(Math.ceil((src.length+9)/64)*64),w=new Uint32Array(64);data.set(src);data[src.length]=128;const view=new DataView(data.buffer),bits=src.length*8;view.setUint32(data.length-8,Math.floor(bits/4294967296));view.setUint32(data.length-4,bits>>>0);const R=(x,n)=>(x>>>n)|(x<<(32-n));
for(let offset=0;offset<data.length;offset+=64){for(let i=0;i<16;i++)w[i]=view.getUint32(offset+i*4);for(let i=16;i<64;i++){const x=w[i-15],y=w[i-2];w[i]=(w[i-16]+(R(x,7)^R(x,18)^(x>>>3))+w[i-7]+(R(y,17)^R(y,19)^(y>>>10)))>>>0;}let [a,b,c,d,e,f,g,h]=H;for(let i=0;i<64;i++){const t1=(h+(R(e,6)^R(e,11)^R(e,25))+((e&f)^(~e&g))+K[i]+w[i])>>>0,t2=((R(a,2)^R(a,13)^R(a,22))+((a&b)^(a&c)^(b&c)))>>>0;h=g;g=f;f=e;e=(d+t1)>>>0;d=c;c=b;b=a;a=(t1+t2)>>>0;}[a,b,c,d,e,f,g,h].forEach((v,i)=>H[i]=(H[i]+v)>>>0);}return H.map(x=>x.toString(16).padStart(8,'0')).join('');}
async function updateFirmware(){const file=document.getElementById('firmware-file').files[0],result=document.getElementById('update-result');if(!file){result.textContent='펌웨어 파일을 선택하세요.';return;}if(!confirm(window.keepRelayPowerOTA?'전원 유지 기능을 사용하는 업데이트입니다. 실제 릴레이 유지가 검증되지 않았다면 PC 전원이 끊길 수 있습니다. 작업을 저장했나요? ESP32와 블루투스가 재시작됩니다.':'PC를 종료하고 C타입을 보조배터리에 연결했나요? ESP32를 업데이트하고 다시 시작합니다.'))return;const button=document.getElementById('update');button.disabled=true;try{result.textContent='파일 확인 중…';const buffer=await file.arrayBuffer(),hash=firmwareHash(buffer),form=new FormData();form.append('firmware',file);result.textContent='Wi-Fi로 업데이트 중…';const response=await fetch('/api/update',{method:'POST',headers:{'X-BC250-UI':'1','X-Firmware-SHA256':hash},body:form});if(!response.ok)throw Error();result.textContent='완료 · 보드가 다시 시작됩니다. 잠시 후 새로고침하세요.';}catch(e){result.textContent='업데이트하지 못했습니다. 서버 전원 상태와 파일을 확인하세요.';}refresh();}

async function restartESP(){
if(!confirm('ESP32를 재부팅할까요? 잠시 관리 화면 연결이 끊깁니다.'))return;
const b=document.getElementById('esp-restart'),out=document.getElementById('esp-restart-result');b.disabled=true;
try{const r=await fetch('/api/esp/restart',{method:'POST',headers:{'Content-Type':'application/json','X-BC250-UI':'1'},body:JSON.stringify({confirm:true})});if(!r.ok)throw Error(r.status===409?'PC를 정상 종료하고 전원 차단 후 다시 시도하세요.':'ESP32 재부팅 요청에 실패했습니다.');out.textContent='ESP32 재부팅 중… 잠시 후 자동으로 다시 연결합니다.';setTimeout(()=>location.reload(),10000);}catch(e){out.textContent=e.message;refresh();}}
let sdLoaded=false;
let sdRefreshing=false;
async function refreshSD(){
if(sdRefreshing)return;sdRefreshing=true;
const el=document.getElementById('sd-state'),summary=document.getElementById('sd-summary');
try{const r=await fetch('/api/sd',{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);const d=await r.json();
if(typeof d.ready!=='boolean')throw Error('잘못된 상태 응답');
el.textContent=d.ready?'SD카드 연결됨':'SD카드 인식 안 됨';el.dataset.state=d.ready?'ready':'error';
summary.textContent=d.error?d.error:d.ready?'카드 인식 정상 · 파일 '+(d.files||[]).length+'개 · 페이지를 열 때 1회 확인':'카드 삽입 상태를 확인한 뒤 SD 다시 인식을 눌러주세요.';
window.sdDefaultsEmbedded=Boolean(d.defaults_embedded);document.getElementById('sd-defaults-state').textContent=d.defaults_embedded?'내장 기본파일: '+d.defaults_version+' · 화면 복원 시 동작 설정 유지':'기본파일 복원은 펌웨어 5.5 설치 후 지원합니다.';
document.getElementById('sd-files').textContent=(d.files||[]).map(f=>f.name+' ('+f.bytes+' bytes)').join(' / ')||'저장된 파일 없음';
if(!sdLoaded){document.getElementById('sd-settings').value=JSON.stringify(d.settings,null,2);sdLoaded=true;}
}catch(e){el.textContent='SD 상태 확인 실패';el.dataset.state='error';summary.textContent='ESP32 응답을 받지 못했습니다. SD 다시 인식 버튼을 눌러 확인하세요.';}
finally{sdRefreshing=false;}}
async function putSD(name,file){const data=new FormData();data.append('file',file,name);const r=await fetch('/api/sd/file?name='+encodeURIComponent(name),{method:'POST',headers:{'X-BC250-UI':'1'},body:data});if(!r.ok){let d=await r.json();throw Error(d.error||'저장 실패');}return r.json();}
async function uploadContent(){const f=document.getElementById('sd-file').files[0];if(!f)return;const result=document.getElementById('sd-result');try{await putSD(f.name,f);result.textContent='파일 저장 완료 · 화면 파일은 새로고침하면 적용됩니다.';sdLoaded=false;await refreshSD();}catch(e){result.textContent=e.message;}}
async function saveSDSettings(){const result=document.getElementById('sd-result');try{const d=JSON.parse(document.getElementById('sd-settings').value);await putSD('settings.json',new Blob([JSON.stringify(d)],{type:'application/json'}));result.textContent='설정 저장·적용 완료 · PC Bluetooth 주소 변경은 정상 종료 후 재시작 시 적용됩니다.';padLoaded=false;sdLoaded=false;await refreshGamepads();await refreshSD();}catch(e){result.textContent='저장하지 못했습니다: '+e.message;}}
async function downloadSDSettings(){try{const r=await fetch('/api/sd/settings',{cache:'no-store'});if(!r.ok)throw Error();const url=URL.createObjectURL(await r.blob()),a=document.createElement('a');a.href=url;a.download='settings.json';a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}catch(e){document.getElementById('sd-result').textContent='다운로드 실패';}}
let sdMountBusy=false;
async function mountCard(){
if(sdMountBusy)return;sdMountBusy=true;
const button=document.getElementById('sd-remount'),out=document.getElementById('sd-result');
button.disabled=true;button.textContent='SD 확인 중…';
try{
const r=await fetch('/api/sd/mount',{method:'POST',headers:{'X-BC250-UI':'1'}});
if(!r.ok)throw Error('SD카드 인식 실패 · 카드와 FAT32 포맷을 확인하세요.');
const d=await r.json();if(d.ready!==true)throw Error('SD카드가 인식되지 않았습니다.');
sdLoaded=false;await refreshSD();
out.textContent='SD카드 다시 인식 완료 · 연결 상태와 설정을 갱신했습니다.';
}catch(e){out.textContent=e.message||'SD카드 확인 실패 · 연결 상태를 확인하세요.';
const state=document.getElementById('sd-state');state.textContent='SD카드 인식 실패';state.dataset.state='error';
document.getElementById('sd-summary').textContent='카드를 확인한 뒤 SD 다시 인식 버튼으로 재시도하세요.';
}finally{sdMountBusy=false;button.disabled=false;button.textContent='SD 다시 인식';}}

let sdFormatting=false;
async function formatSD(){
if(sdFormatting)return;sdFormatting=true;const result=document.getElementById('sd-result');
try{
const infoResponse=await fetch('/api/sd/format-info',{method:'POST',headers:{'X-BC250-UI':'1'}});if(!infoResponse.ok)throw Error('SD카드를 인식하지 못했습니다.');
const info=await infoResponse.json();
const typed=prompt('포맷 대상: ESP32 슬롯 SD카드 '+(info.bytes/1073741824).toFixed(2)+' GiB\n카드 전체 파일이 삭제됩니다. 진행하려면 FORMAT SD를 입력하세요. 확인 후 카드를 교체하지 마세요.');
if(typed!=='FORMAT SD'){result.textContent='포맷을 취소했습니다.';return;}
result.textContent='FAT32 포맷 중… 전원을 끄거나 카드를 빼지 마세요.';
const r=await fetch('/api/sd/format',{method:'POST',headers:{'Content-Type':'application/json','X-BC250-UI':'1'},body:JSON.stringify({token:info.token,confirm:typed})});if(!r.ok)throw Error('포맷 또는 재인식 실패 · 연결과 카드 상태를 확인하세요.');
result.textContent='FAT32 포맷 완료 · 현재 설정은 보존했습니다. 기본 화면을 SD에 생성 버튼으로 웹 화면을 복원하세요.';sdLoaded=false;await refreshSD();
}catch(e){result.textContent=e.message;}finally{sdFormatting=false;}}
async function seedCard(overwrite=false){try{if(overwrite&&!window.sdDefaultsEmbedded)throw Error('기본화면 복원은 펌웨어 5.5부터 지원합니다.');if(overwrite&&!confirm('SD 화면 파일을 펌웨어 기본값으로 복원할까요? 기존 파일은 .bak으로 보관하고 동작 설정은 유지합니다.'))return;const r=await fetch('/api/sd/seed',{method:'POST',headers:{'X-BC250-UI':'1','Content-Type':'application/json'},body:JSON.stringify({overwrite})});if(!r.ok)throw Error('파일 생성 실패 · SD 상태를 확인하세요.');document.getElementById('sd-result').textContent=overwrite?'기본화면을 복원했습니다. 페이지를 새로고침하세요.':'없는 기본파일을 설치했습니다. 기존 파일은 유지됩니다.';sdLoaded=false;await refreshSD();}catch(e){document.getElementById('sd-result').textContent=e.message;}}
refreshSD();refresh();refreshGamepads();window.padRefreshAt=performance.now();setInterval(()=>{const now=performance.now();if(!window.metricsRefreshBusy&&now-(window.metricsRefreshAt||0)>=metricsInterval)refresh();if(now-window.padRefreshAt>=5000){window.padRefreshAt=now;refreshGamepads();}},250);


function updateOverview(s){
const put=(id,text)=>{const e=document.getElementById(id);if(e)e.textContent=text;};
if(s.firmware)put('version-label',s.firmware.replace('bc250-monitor-','펌웨어 '));
put('transport-label',s.metrics_transport==='usb'?'모니터링 · USB':'모니터링 · Wi-Fi');
put('connection-label',s.agent_online?'시스템 연결됨':'시스템 수신 대기');
const chip=document.querySelector('.status-chip');if(chip)chip.dataset.state=s.agent_online?'online':'offline';
put('display-state',typeof s.display_awake==='boolean'?(s.display_awake?'화면 켜짐':'화면 꺼짐 · 터치하면 다시 켜집니다'):'장치 연결 확인 필요');
for(const id of ['display-off','display-on']){const e=document.getElementById(id);if(e)e.disabled=typeof s.display_awake!=='boolean';}
put('ota-capability',s.ota_in_progress?'업데이트 진행 중':s.ota_keep_power_supported?'전원 유지 OTA 지원 · 실물 검증 필요':'PC 정상 종료 후 보조전원으로 업데이트');
}

let usbFeatureBusy=false;
async function setUSBFeature(feature,enabled){
 if(usbFeatureBusy)return;usbFeatureBusy=true;
 const result=document.getElementById('usb-test-result')||document.getElementById('usb-bt-result');result.textContent='설정 요청 중…';
 try{
  const response=await fetch('/api/usb-features',{method:'POST',headers:{'Content-Type':'application/json','X-BC250-UI':'1'},body:JSON.stringify({feature,enabled})});
  const data=await response.json();if(!response.ok)throw Error(data.error==='bluetooth_transport_required'?'USB 블루투스를 먼저 켜세요. 블루투스를 끄려면 다른 두 기능을 먼저 끄세요.':data.error==='usb_not_ready'?'USB 통신 준비 중입니다. 잠시 후 다시 시도하세요.':'설정 요청 실패: '+(data.error||response.status));
  result.textContent='요청 완료 · 아래 실제 수신 상태를 확인하세요.';
 }catch(error){result.textContent=error.message;}finally{usbFeatureBusy=false;refresh();}
}
function showUSBFeatures(s){
 const flags=s.usb_features,el=id=>document.getElementById(id);if(!el('usb-test-card'))return;
 const available=!!flags&&s.relay_output==='HIGH'&&!s.ota_in_progress;
 for(const feature of ['bluetooth','monitoring','internet']){
  const on=!!flags?.[feature];el('usb-'+feature+'-state').textContent=on?'설정 ON':'OFF';
  el('usb-'+feature+'-on').disabled=usbFeatureBusy||!available||on||(feature!=='bluetooth'&&!flags.bluetooth);
  el('usb-'+feature+'-off').disabled=usbFeatureBusy||!available||!on||(feature==='bluetooth'&&(flags.monitoring||flags.internet));
 }
 const net=s.emergency_network;
 el('usb-test-detail').textContent='USB 엔진: '+(s.usb_bluetooth?.active?'동작':'대기')+' · 모니터 수신: '+(s.metrics_transport==='usb'?'USB':'Wi-Fi')+' · 비상 인터넷 PC 연결: '+(net?.pc_ready?'준비됨':'대기')+' · USB 송신/수신 패킷: '+(s.usb_bluetooth?.host_packets||0)+' / '+(s.usb_bluetooth?.controller_packets||0);
}
