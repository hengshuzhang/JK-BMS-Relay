#include "WebManager.h"

#include <ArduinoJson.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <driver/temp_sensor.h>
#include "AppConfig.h"
#include "BmsCentral.h"
#include "BmsNimbleRelay.h"
#include "NvsStore.h"

namespace {
bool coreTemperatureSensorReady = false;

void initCoreTemperatureSensor() {
    if (coreTemperatureSensorReady) return;

    temp_sensor_config_t config;
    config.dac_offset = TSENS_DAC_L2;  // -10 to 80 degrees Celsius.
    config.clk_div = 6;
    const esp_err_t configResult = temp_sensor_set_config(config);
    if (configResult != ESP_OK) {
        APP_LOGF("[Temperature] sensor configuration failed: %d\n",
                      static_cast<int>(configResult));
        return;
    }

    const esp_err_t startResult = temp_sensor_start();
    if (startResult != ESP_OK) {
        APP_LOGF("[Temperature] sensor start failed: %d\n",
                      static_cast<int>(startResult));
        return;
    }
    coreTemperatureSensorReady = true;
    APP_LOGLN("[Temperature] ESP32 core temperature sensor enabled");
}

bool readCoreTemperature(float& celsius) {
    if (!coreTemperatureSensorReady) return false;
    return temp_sensor_read_celsius(&celsius) == ESP_OK;
}

const char kIndexHtml[] PROGMEM = R"HTML(
<!doctype html>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S3 BMS Relay</title>
<style>
body{font-family:system-ui,sans-serif;max-width:760px;margin:20px auto;padding:0 14px;color:#222}
button{margin:6px 4px 6px 0;padding:7px 14px}
input{padding:7px;width:220px;max-width:70%}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:12px;margin-top:16px}
.card{border:1px solid #ccc;border-radius:8px;padding:12px;background:#fafafa}
.card h3{margin:0 0 8px}.ok{color:#16803c}.off{color:#777}.error{color:#b42318}
.signal-card{min-width:0}.signal-card.locked{opacity:.58}.signal-card h3{display:flex;justify-content:space-between;align-items:center}.signal-state{margin:4px 0}.signal-controls{display:flex;align-items:center;gap:8px;flex-wrap:wrap}.signal-controls input{box-sizing:border-box;width:180px;max-width:100%}
button:disabled,input:disabled{cursor:not-allowed}
</style>
<div id="status-page">
<h2>BMS Relay</h2>
<p>AP: CAR &nbsp; IP: 192.168.4.1</p>
<label>BMS MAC <input id="mac" type="text" autocomplete="off" spellcheck="false" placeholder="AA:BB:CC:DD:EE:FF 或 AABBCCDDEEFF"></label>
<button onclick="connectBms()">连接</button><button onclick="disconnectBms()">断开</button>
<p>手机连接：<strong id="phone-count">0</strong> / <span id="phone-max">-</span></p>
<p>核心温度：<strong id="core-temperature">读取中...</strong></p>
<div class="card" id="bms-card">
  <h3>BMS 连接状态</h3>
  <div>目标 MAC：<span id="bms-mac">-</span></div>
  <div>当前状态：<strong id="bms-state">读取中...</strong></div>
  <div id="bms-detail">正在读取 BMS 状态...</div>
  <div id="bms-error" class="error"></div>
</div>
<div class="card" id="signal-config">
  <h3>虚拟广播</h3>
  <p class="off">固定提供两个广播，均使用 FFE0/FFE1 服务。可修改未连接广播的名称；每个名称最多12字节。</p>
  <div id="signal-name-list" class="grid"></div>
  <button id="save-signals" type="button" onclick="saveSignals()">保存广播名称</button>
  <div id="signal-lock-status" class="off"></div>
  <div id="signal-config-status" class="off"></div>
</div>
<div class="card" id="ota-card">
  <h3>固件 OTA 更新</h3>
  <form id="ota-form" enctype="multipart/form-data">
    <input id="ota-file" type="file" name="firmware" accept=".bin,application/octet-stream" required>
    <button type="submit">上传并重启</button>
  </form>
  <div id="ota-status" class="off">请选择新的 .bin 固件文件</div>
</div>
</div>
<script>
function setBmsStatus(data){
  const state=document.getElementById('bms-state');
  const detail=document.getElementById('bms-detail');
  const error=document.getElementById('bms-error');
  const mac=data.mac||'未填写';
  const labels=['未连接','正在扫描目标 BMS','正在建立 BMS 连接','BMS 已连接','BMS 已断开，等待自动重连'];
  const connected=!!data.connected;
  let text=connected?'BMS 已连接':(labels[data.state]||'状态未知');
  let detailText='';
  if(data.state===0) detailText='请填写 BMS MAC 地址后点击“连接”';
  else if(data.state===1) detailText='正在查找目标设备：'+mac;
  else if(data.state===2) detailText='已找到目标设备，正在连接并检查 FFE0/FFE1';
  else if(data.state===3) detailText='FFE0/FFE1 已找到，等待虚拟客户端订阅或写入';
  else if(data.state===4) detailText='设备暂时断开，系统将在几秒后自动重试';
  state.textContent=text;
  state.className=connected?'ok':(data.error?'error':'off');
  detail.textContent=detailText;
  error.textContent=data.error?('错误：'+data.error):'';
  document.getElementById('bms-mac').textContent=mac;
}
let signalConfigDirty=false;
let phoneConnections=0;
let signalConnectionStates=[];
function updateSignalEditingState(){
  const cards=document.querySelectorAll('.signal-card');
  cards.forEach((card,index)=>{
    const connected=!!signalConnectionStates[index];
    card.classList.toggle('locked',connected);
    const input=card.querySelector('.signal-name');
    if(input) input.disabled=connected;
  });
  document.getElementById('save-signals').disabled=false;
  const lockStatus=document.getElementById('signal-lock-status');
  const connectedCount=signalConnectionStates.filter(Boolean).length;
  lockStatus.textContent=connectedCount?'已连接的广播名称已锁定，未连接的广播可以编辑。':'';
  lockStatus.className='off';
}
function signalStateText(signal){
  if(signal.connected) return '已连接手机，广播配置已锁定';
  return signal.advertising?'已广播，可连接':(signal.available?'已取消广播':'未创建');
}
function renderSignalConfig(signals){
  const items=signals.items||[];
  const list=document.getElementById('signal-name-list');
  signalConnectionStates=items.map(signal=>!!signal.connected);
  list.innerHTML='';
  items.forEach((signal,index)=>{
    const card=document.createElement('div');
    card.className='card signal-card';

    const title=document.createElement('h3');
    title.textContent='广播 '+(index+1);
    card.appendChild(title);

    const state=document.createElement('div');
    state.className=signal.connected?'signal-state off':(signal.advertising?'signal-state ok':'signal-state off');
    state.textContent=signalStateText(signal);
    card.appendChild(state);

    const notify=document.createElement('div');
    notify.className='off';
    notify.textContent='Notify：'+(signal.subscribed?'已订阅':'未订阅（可读取）');
    card.appendChild(notify);

    const controls=document.createElement('div');
    controls.className='signal-controls';
    const label=document.createElement('label');
    label.textContent='广播名';
    const input=document.createElement('input');
    input.className='signal-name';
    input.maxLength=12;
    input.value=signal.name||'';
    input.disabled=!!signal.connected;
    input.autocomplete='off';
    input.addEventListener('input',()=>{signalConfigDirty=true;});
    label.appendChild(input);
    controls.appendChild(label);
    card.appendChild(controls);
    list.appendChild(card);
  });
  updateSignalEditingState();
}
async function saveSignals(){
  const status=document.getElementById('signal-config-status');
  const names=Array.from(document.querySelectorAll('.signal-name')).map(input=>input.value.trim());
  status.textContent='正在保存...'; status.className='off';
  try{
    const response=await fetch('/signals',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({names:names})});
    const message=await response.text();
    if(!response.ok){
      const detail=response.status===409?'已连接的广播名称不能修改':(message||'保存失败');
      throw new Error(detail);
    }
    signalConfigDirty=false;
    status.textContent='保存成功，虚拟广播已更新'; status.className='ok';
    refresh();
  }catch(error){status.textContent='保存失败：'+error.message;status.className='error';}
}
async function refresh(){
  try{
    const data=await (await fetch('/status',{cache:'no-store'})).json();
    // Do not overwrite a MAC address the user is editing. The status poll runs
    // every two seconds, so an unconditional assignment loses manual changes.
    if(!macInputDirty && document.activeElement!==macInput) macInput.value=data.mac||'';
    phoneConnections=Number(data.phone_connections)||0;
    signalConnectionStates=(data.signals.items||[]).map(signal=>!!signal.connected);
    document.getElementById('phone-count').textContent=phoneConnections;
    document.getElementById('phone-max').textContent=data.signals.signal_count||0;
    const coreTemperature=document.getElementById('core-temperature');
    if(data.core_temperature_available && typeof data.core_temperature_c==='number'){
      coreTemperature.textContent=data.core_temperature_c.toFixed(1)+' °C';
      coreTemperature.className='ok';
    }else{
      coreTemperature.textContent='不可用';
      coreTemperature.className='off';
    }
    setBmsStatus(data);
    updateSignalEditingState();
    if(!signalConfigDirty) renderSignalConfig(data.signals);
  }catch(error){console.error('状态读取失败：',error);}
}
async function connectBms(){
  const status=document.getElementById('bms-state');
  const error=document.getElementById('bms-error');
  status.textContent='正在连接...'; status.className='off'; error.textContent='';
  try{
    const response=await fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mac:macInput.value})});
    const message=await response.text();
    if(!response.ok){ status.textContent='连接请求失败'; status.className='error'; error.textContent='（'+message+'）'; }
    else macInputDirty=false;
  }catch(ex){ status.textContent='连接请求失败'; status.className='error'; error.textContent='（网络错误）'; }
  refresh();
}
async function disconnectBms(){
  await fetch('/disconnect',{method:'POST'});
  refresh();
}
const otaForm=document.getElementById('ota-form');
const otaFile=document.getElementById('ota-file');
const otaStatus=document.getElementById('ota-status');
otaForm.addEventListener('submit',async event=>{
  event.preventDefault();
  const file=otaFile.files[0];
  if(!file){otaStatus.textContent='请选择固件文件';otaStatus.className='error';return;}
  const button=otaForm.querySelector('button');
  button.disabled=true;
  otaStatus.textContent='正在上传固件，请勿断电...';
  otaStatus.className='off';
  try{
    const response=await fetch('/update',{method:'POST',body:new FormData(otaForm)});
    const message=await response.text();
    if(!response.ok) throw new Error(message||'OTA 更新失败');
    otaStatus.textContent='固件上传成功，设备正在重启...';
    otaStatus.className='ok';
  }catch(error){
    otaStatus.textContent='OTA 更新失败：'+error.message;
    otaStatus.className='error';
    button.disabled=false;
  }
});
const macInput=document.getElementById('mac');
let macInputDirty=false;
macInput.addEventListener('input',()=>{macInputDirty=true;});
refresh();setInterval(refresh,500);
</script>
)HTML";

}

WebManager::WebManager(BmsCentral& central, JkBmsNimbleRelay& relay, NvsStore& store)
    : central_(central), relay_(relay), store_(store) {}

bool WebManager::startSoftAp() {
    wifiSleeping_ = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));
    if (!WiFi.softAP(AppConfig::kApSsid, AppConfig::kApPassword)) {
        APP_LOGLN("[WiFi] SoftAP start failed");
        nextWifiRestoreAt_ = millis() + 1000U;
        return false;
    }

    // esp_wifi_set_max_tx_power() uses units of 0.25 dBm: 7 dBm = 28.
    const esp_err_t txPowerResult = esp_wifi_set_max_tx_power(28);
    const esp_err_t psResult = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    wifi_config_t apConfig{};
    esp_err_t beaconResult = esp_wifi_get_config(WIFI_IF_AP, &apConfig);
    if (beaconResult == ESP_OK) {
        // beacon_interval is measured in TU (1 TU = 1024 us).
        apConfig.ap.beacon_interval = 1000;
        beaconResult = esp_wifi_set_config(WIFI_IF_AP, &apConfig);
    }
    APP_LOGF("[WiFi] SoftAP %s, IP=%s, TX power=7 dBm, PS=MIN_MODEM, beacon=1000TU (~1.024s)\n",
                  AppConfig::kApSsid, WiFi.softAPIP().toString().c_str());
    if (txPowerResult != ESP_OK) {
        APP_LOGF("[WiFi] TX power setup failed: %d\n",
                      static_cast<int>(txPowerResult));
    }
    if (psResult != ESP_OK) {
        APP_LOGF("[WiFi] modem sleep setup failed: %d\n", static_cast<int>(psResult));
    }
    if (beaconResult != ESP_OK) {
        APP_LOGF("[WiFi] beacon interval setup failed: %d\n",
                      static_cast<int>(beaconResult));
    }
    wifiSleeping_ = false;
    nextWifiRestoreAt_ = 0;
    return true;
}

void WebManager::updatePowerSaving() {
    const uint32_t now = millis();
    if (!central_.connected()) {
        bmsConnectedSince_ = 0;
        if (wifiSleeping_ && static_cast<int32_t>(now - nextWifiRestoreAt_) >= 0) {
            if (startSoftAp()) {
                // WiFi.mode(WIFI_OFF) invalidates the previous listen socket.
                // Re-open it after the AP has been brought back up; routes stay
                // registered and must not be registered a second time.
                server_.begin();
                APP_LOGLN("[WiFi] WebServer listener restored after AP restart");
            }
        }
        return;
    }

    if (wifiSleeping_) return;
    if (bmsConnectedSince_ == 0) bmsConnectedSince_ = now;

    constexpr unsigned long kStableConnectionBeforeWifiOff = 10UL * 60UL * 1000UL;
    const bool stable = now - bmsConnectedSince_ >= kStableConnectionBeforeWifiOff;
    const bool apInUse = WiFi.softAPgetStationNum() > 0;
    if (stable && !apInUse) {
        WiFi.mode(WIFI_OFF);
        wifiSleeping_ = true;
        nextWifiRestoreAt_ = 0;
        APP_LOGLN("[WiFi] disabled after 10 minutes of stable BMS connection");
    }
}

void WebManager::begin() {
    initCoreTemperatureSensor();
    startSoftAp();
    server_.on("/", HTTP_GET, [this]() { root(); });
    server_.on("/status", HTTP_GET, [this]() { status(); });
    server_.on("/connect", HTTP_POST, [this]() { connect(); });
    server_.on("/disconnect", HTTP_POST, [this]() { disconnect(); });
    server_.on("/signals", HTTP_POST, [this]() { updateSignals(); });
    server_.on("/update", HTTP_POST,
               [this]() {
                   const bool ok = otaUploadStarted_ && otaUploadComplete_ && !Update.hasError();
                   server_.sendHeader("Connection", "close");
                   server_.send(ok ? 200 : 500, "text/plain; charset=UTF-8",
                                ok ? "OK" : "OTA update failed");
                   if (ok) {
                       delay(250);
                       ESP.restart();
                   }
               },
               [this]() {
                   HTTPUpload& upload = server_.upload();
                   if (upload.status == UPLOAD_FILE_START) {
                       otaUploadStarted_ = Update.begin(UPDATE_SIZE_UNKNOWN);
                       otaUploadComplete_ = false;
                       APP_LOGF("[OTA] start: %s\n", upload.filename.c_str());
                       if (!otaUploadStarted_) {
                           APP_LOG_UPDATE_ERROR(Update);
                       }
                   } else if (upload.status == UPLOAD_FILE_WRITE) {
                       if (!otaUploadStarted_ ||
                           Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                           otaUploadComplete_ = false;
                           APP_LOG_UPDATE_ERROR(Update);
                       }
                   } else if (upload.status == UPLOAD_FILE_END) {
                       otaUploadComplete_ = otaUploadStarted_ && Update.end(true);
                       if (!otaUploadComplete_) {
                           APP_LOG_UPDATE_ERROR(Update);
                       } else {
                           APP_LOGF("[OTA] complete: %u bytes\n",
                                         static_cast<unsigned>(upload.totalSize));
                       }
                   } else if (upload.status == UPLOAD_FILE_ABORTED) {
                       otaUploadComplete_ = false;
                       if (otaUploadStarted_) Update.abort();
                       APP_LOGLN("[OTA] aborted");
                   }
               });
    server_.on("/relay/status", HTTP_GET, [this]() { status(); });
    server_.onNotFound([this]() { server_.send(404, "text/plain; charset=UTF-8", "Not found"); });
    server_.begin();
}

void WebManager::handle() {
    updatePowerSaving();
    if (!wifiSleeping_) server_.handleClient();
}

void WebManager::root() {
    server_.send_P(200, "text/html; charset=UTF-8", kIndexHtml);
}

void WebManager::status() {
    // Keep the response document out of the WebServer task stack. The status
    // endpoint is polled frequently, so also stream JSON directly instead of
    // allocating a temporary String for every response.
    static StaticJsonDocument<3072> document;
    document.clear();

    const uint32_t now = millis();
    float coreTemperature = 0.0f;
    const bool coreTemperatureAvailable = readCoreTemperature(coreTemperature);
    document["core_temperature_available"] = coreTemperatureAvailable;
    if (coreTemperatureAvailable) {
        document["core_temperature_c"] = coreTemperature;
    } else {
        document["core_temperature_c"] = nullptr;
    }
    document["mode"] = "transparent_raw_forward";
    document["state"] = static_cast<int>(central_.state());
    document["mac"] = central_.targetMac();
    document["connected"] = central_.connected();
    document["upstream_subscription_requested"] = central_.upstreamSubscriptionRequested();
    document["upstream_subscription_pending"] = central_.upstreamSubscriptionPending();
    document["upstream_subscribed"] = central_.upstreamSubscribed();
    document["upstream_notify_count"] = central_.upstreamNotifyCount();
    document["upstream_last_notify_length"] = central_.lastUpstreamNotifyLength();
    if (central_.hasUpstreamNotify()) {
        document["upstream_last_notify_age_ms"] = now - central_.lastUpstreamNotifyAt();
    } else {
        document["upstream_last_notify_age_ms"] = nullptr;
    }
    document["upstream_subscription_attempts"] = central_.upstreamSubscriptionAttemptCount();
    document["upstream_subscription_successes"] = central_.upstreamSubscriptionSuccessCount();
    document["upstream_subscription_failures"] = central_.upstreamSubscriptionFailureCount();
    document["upstream_unsubscribe_failures"] = central_.upstreamUnsubscribeFailureCount();
    document["free_heap"] = ESP.getFreeHeap();
    document["min_free_heap"] = ESP.getMinFreeHeap();
    document["error"] = central_.lastError();
    document["relay_running"] = relay_.running();
    document["relay_bms_available"] = relay_.bmsConnectionAvailable();
    document["relay_advertisement_start_pending"] = relay_.advertisementStartPending();
    document["phone_connected"] = relay_.phoneConnected();
    document["phone_connections"] = relay_.phoneConnectionCount();
    document["phone_max_connections"] = AppConfig::kMaxPhoneConnections;
    document["notify_enabled"] = relay_.notificationsEnabled();
    JsonObject relayHealth = document.createNestedObject("relay_health");
    relayHealth["cached_bms_frames"] = relay_.cachedBmsFrameCount();
    relayHealth["latest_data_length"] = relay_.latestDataLength();
    relayHealth["downstream_write_successes"] = relay_.downstreamWriteSuccessCount();
    relayHealth["downstream_write_failures"] = relay_.downstreamWriteFailureCount();
    relayHealth["downstream_notify_successes"] = relay_.downstreamNotifySuccessCount();
    relayHealth["downstream_notify_failures"] = relay_.downstreamNotifyFailureCount();
    relayHealth["downstream_forced_disconnects"] = relay_.downstreamForcedDisconnectCount();
    if (relay_.lastDownstreamNotifyAt() != 0) {
        relayHealth["last_downstream_notify_age_ms"] = now - relay_.lastDownstreamNotifyAt();
    } else {
        relayHealth["last_downstream_notify_age_ms"] = nullptr;
    }
    document["relay_name"] = relay_.name();
    const IPAddress apIp = WiFi.softAPIP();
    char softApIp[16] = {};
    snprintf(softApIp, sizeof(softApIp), "%u.%u.%u.%u", apIp[0], apIp[1], apIp[2], apIp[3]);
    document["softap_ip"] = softApIp;

    JsonObject signals = document.createNestedObject("signals");
    signals["advertising"] = relay_.advertising();
    signals["signal_count"] = relay_.signalCount();
    signals["max_count"] = AppConfig::kMaxVirtualSignals;
    JsonArray items = signals.createNestedArray("items");
    for (uint8_t i = 0; i < relay_.signalCount(); ++i) {
        JsonObject item = items.createNestedObject();
        item["index"] = i;
        item["name"] = relay_.signalName(i);
        item["service"] = AppConfig::kBmsServiceUuid;
        item["characteristic"] = AppConfig::kBmsCharacteristicUuid;
        item["available"] = relay_.signalAvailable(i);
        item["advertising"] = relay_.signalAdvertising(i);
        item["connected"] = relay_.signalConnected(i);
        item["editable"] = !relay_.signalConnected(i);
        item["subscribed"] = relay_.notificationsEnabled();
        item["properties"] = "read,write,write_nr,notify";
    }

    if (document.overflowed()) {
        server_.send(500, "text/plain; charset=UTF-8", "Status response too large");
        return;
    }
    // WebServer owns response completion. Reuse a single output buffer so the
    // complete JSON is sent through server_.send() without per-request heap
    // churn or an incomplete manually streamed HTTP body.
    static String output;
    const size_t outputLength = measureJson(document);
    if (!output.reserve(outputLength)) {
        server_.send(500, "text/plain; charset=UTF-8", "Status response buffer unavailable");
        return;
    }
    output.remove(0);
    serializeJson(document, output);
    server_.send(200, "application/json; charset=UTF-8", output);
}

void WebManager::connect() {
    StaticJsonDocument<128> document;
    if (deserializeJson(document, server_.arg("plain"))) {
        server_.send(400, "text/plain; charset=UTF-8", "Bad JSON");
        return;
    }
    String mac = document["mac"] | "";
    String canonicalMac;
    if (!BmsCentral::normalizeMac(mac, canonicalMac)) {
        server_.send(400, "text/plain; charset=UTF-8", "Invalid MAC: use AA:BB:CC:DD:EE:FF or AABBCCDDEEFF");
        return;
    }
    if (!central_.connectTo(canonicalMac)) {
        server_.send(503, "text/plain; charset=UTF-8", "Cannot schedule connection");
        return;
    }
    if (!store_.saveBmsMac(canonicalMac)) {
        APP_LOGLN("[NVS] Failed to save BMS MAC");
    }
    server_.send(202, "text/plain; charset=UTF-8", "connection scheduled");
}

void WebManager::disconnect() {
    central_.disconnect();
    server_.send(200, "text/plain; charset=UTF-8", "disconnected");
}

void WebManager::updateSignals() {
    StaticJsonDocument<512> document;
    if (deserializeJson(document, server_.arg("plain"))) {
        server_.send(400, "text/plain; charset=UTF-8", "Bad JSON");
        return;
    }

    JsonArray values = document["names"].as<JsonArray>();
    if (values.isNull() || values.size() != AppConfig::kDefaultVirtualSignalCount) {
        server_.send(400, "text/plain; charset=UTF-8", "Names must contain exactly 2 signals");
        return;
    }

    String names[AppConfig::kMaxVirtualSignals];
    String previousNames[AppConfig::kMaxVirtualSignals];
    for (uint8_t i = 0; i < AppConfig::kDefaultVirtualSignalCount; ++i) {
        names[i] = values[i].as<String>();
        names[i].trim();
        if (names[i].isEmpty() || names[i].length() > AppConfig::kVirtualSignalNameMaxLength) {
            server_.send(400, "text/plain; charset=UTF-8", "Signal names must be 1 to 12 bytes");
            return;
        }
        previousNames[i] = relay_.signalName(i);
        if (relay_.signalConnected(i) && names[i] != previousNames[i]) {
            server_.send(409, "text/plain; charset=UTF-8",
                         "已连接的广播名称不能修改");
            return;
        }
    }

    // Apply only after all input and connection checks pass. Persist after a
    // successful runtime update so NVS never advertises a configuration that
    // the current relay explicitly rejected.
    if (!relay_.configureSignals(AppConfig::kDefaultVirtualSignalCount, names)) {
        server_.send(409, "text/plain; charset=UTF-8",
                     "已连接的广播名称不能修改，或广播配置无法应用");
        return;
    }
    if (!store_.saveVirtualSignals(AppConfig::kDefaultVirtualSignalCount, names)) {
        // Preferences writes are not transactional. Restore both sides on a
        // failed save where possible, rather than leaving the current session
        // and the next boot with different names.
        const bool runtimeRestored = relay_.configureSignals(
            AppConfig::kDefaultVirtualSignalCount, previousNames);
        const bool nvsRestored = store_.saveVirtualSignals(
            AppConfig::kDefaultVirtualSignalCount, previousNames);
        server_.send(500, "text/plain; charset=UTF-8",
                     runtimeRestored && nvsRestored
                         ? "Failed to save virtual signal names; previous names restored"
                         : "Failed to save virtual signal names; restart before changing names again");
        return;
    }
    server_.send(200, "text/plain; charset=UTF-8", "virtual signal names updated");
}
