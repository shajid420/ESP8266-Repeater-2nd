/*
  ===========================================================================
  ESP8266 Wi-Fi Repeater (STA + AP, with REAL lwIP NAPT routing)
  ===========================================================================

  This is a NAT-router-style repeater, NOT a Layer-2/WDS transparent
  repeater (the ESP8266 SDK does not expose WDS/4-address-frame support,
  so a transparent same-SSID repeater is not achievable with this chip).

  Architecture:
    Upstream router  <---- STA link ---->  ESP8266  <---- AP link ---->  Phone/laptop
                                          (NAPT / NAT here)

  The ESP8266 joins your existing Wi-Fi as a normal client (STA), and at
  the same time runs its own access point (AP) with a different SSID and
  a different IP subnet. lwIP's NAPT (Network Address & Port Translation)
  rewrites packets between the two interfaces so AP clients get real
  internet access through the STA link -- this is the same architecture
  used by "travel routers" / MiFi devices, and is what actually forwards
  traffic (as opposed to just creating an AP that goes nowhere).

  ---------------------------------------------------------------------
  REQUIRED Arduino IDE build settings (Tools menu) -- read this first:
  ---------------------------------------------------------------------
    Board:        NodeMCU 1.0 (ESP-12E Module)   [or "Generic ESP8266 Module"
                   if you're using an ESP-07S board -- see README]
    Flash Size:   4MB (FS:none, or any split that keeps >=1MB for sketch)
    lwIP Variant: "v2 Higher Bandwidth"                <-- MUST NOT be a
                                                            "(no features)"
                                                            or "IPv6" variant.
                  The "(no features)" variants strip out IP forwarding/NAT
                  support entirely -- the sketch will compile but NAPT will
                  silently do nothing. "IPv6" variants also disable the
                  NAPT code path used here (LWIP_IPV6 must be 0).
    CPU Frequency: 160 MHz (reduces latency/jitter under load)
    Debug port:    Disabled (saves RAM)

  Libraries used are all part of the stock esp8266/Arduino core (>=2.6.0,
  tested against 3.x) -- no third-party NAT library needed.
  ===========================================================================
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <string.h>
#include <lwip/napt.h>
#include <lwip/dns.h>

#if !LWIP_FEATURES || LWIP_IPV6
#error "Wrong lwIP variant selected. Tools > lwIP Variant must be a non-IPv6, " \
       "non-'(no features)' variant (e.g. 'v2 Higher Bandwidth') so NAPT is available."
#endif

// ---------------------------------------------------------------------
// NAPT table sizing -- tuned for ONE TO A FEW client devices.
//   NAPT_MAX_ENTRIES  = max simultaneous NAT sessions (TCP+UDP flows).
//                        A phone doing a voice call + YouTube typically
//                        uses well under 100 flows; 512 leaves headroom
//                        for a browser tab or two without wasting RAM.
//   NAPT_MAX_PORTMAPS = max port-forwarding rules (not used by this
//                        sketch; kept small on purpose).
// Raise NAPT_MAX_ENTRIES (e.g. to 1000, the upstream example's default)
// if pages fail to load because many parallel connections are in use.
// ---------------------------------------------------------------------
#define NAPT_MAX_ENTRIES   512
#define NAPT_MAX_PORTMAPS  10

// Max simultaneous Wi-Fi clients on the repeater's access point.
#define MAX_AP_CLIENTS     4

// ---------------------------------------------------------------------
// Defaults -- only used on first boot / if EEPROM config is invalid.
// Everything below can be changed later from the web config page.
// ---------------------------------------------------------------------
#define DEFAULT_ROUTER_SSID  "YourHomeRouterSSID"
#define DEFAULT_ROUTER_PASS  "YourHomeRouterPassword"
#define DEFAULT_AP_SSID      "ESP8266-Repeater"
#define DEFAULT_AP_PASS      "repeater123"     // WPA2 requires >= 8 chars
#define DEFAULT_AP_CHANNEL   6

#define CONFIG_MAGIC 0x52504931UL // "RPI1" -- bump this if you change the struct

struct Config {
  uint32_t magic;
  char routerSsid[33];
  char routerPass[65];
  char apSsid[33];
  char apPass[65];
  uint8_t apChannel;
};

Config cfg;
ESP8266WebServer server(80);

bool naptStarted = false;

// reconnect / watchdog state
unsigned long lastStaCheckMs = 0;
unsigned long staDownSinceMs = 0;
bool staWasConnected = false;
const unsigned long STA_CHECK_INTERVAL_MS = 5000;         // poll every 5s
const unsigned long STA_MAX_DOWNTIME_MS   = 120UL * 1000; // hard restart after 2 min down

// =======================================================================
// EEPROM config
// =======================================================================
void loadConfig() {
  EEPROM.begin(512);
  EEPROM.get(0, cfg);
  if (cfg.magic != CONFIG_MAGIC) {
    memset(&cfg, 0, sizeof(cfg));
    cfg.magic = CONFIG_MAGIC;
    strlcpy(cfg.routerSsid, DEFAULT_ROUTER_SSID, sizeof(cfg.routerSsid));
    strlcpy(cfg.routerPass, DEFAULT_ROUTER_PASS, sizeof(cfg.routerPass));
    strlcpy(cfg.apSsid,     DEFAULT_AP_SSID,     sizeof(cfg.apSsid));
    strlcpy(cfg.apPass,     DEFAULT_AP_PASS,     sizeof(cfg.apPass));
    cfg.apChannel = DEFAULT_AP_CHANNEL;
    EEPROM.put(0, cfg);
    EEPROM.commit();
    Serial.println(F("[CFG] No valid config found, wrote defaults."));
  } else {
    Serial.println(F("[CFG] Loaded config from EEPROM."));
  }
}

void saveConfig() {
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// =======================================================================
// Wi-Fi bring-up
// =======================================================================
void startStation() {
  WiFi.begin(cfg.routerSsid, cfg.routerPass);
  Serial.printf("[STA] Connecting to '%s'...\n", cfg.routerSsid);
}

void startAP() {
  // Private /24 chosen to avoid colliding with common home router ranges
  // (192.168.0.x, 192.168.1.x) so double-NAT routing doesn't get confused.
  IPAddress apIP(192, 168, 44, 1);
  IPAddress apGW(192, 168, 44, 1);
  IPAddress apMask(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apGW, apMask);
  // max 4 simultaneous AP clients -- keeps NAT/CPU load appropriate for
  // "one or a few devices"; raise if you really need more.
  WiFi.softAP(cfg.apSsid, cfg.apPass, cfg.apChannel, false, MAX_AP_CLIENTS);
  Serial.printf("[AP] '%s' up on channel %d, IP %s\n",
                cfg.apSsid, cfg.apChannel, WiFi.softAPIP().toString().c_str());
}

// Must be called only after STA is actually connected (NAPT needs to know
// the real upstream DNS server, obtained via DHCP from the router).
void startNaptIfReady() {
  if (naptStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;

  // Point AP-side DHCP clients at the router's real DNS server instead of
  // at the ESP8266 itself.
  auto &dhcp = WiFi.softAPDhcpServer();
  dhcp.setDns(WiFi.dnsIP(0));

  err_t ret = ip_napt_init(NAPT_MAX_ENTRIES, NAPT_MAX_PORTMAPS);
  Serial.printf("[NAPT] ip_napt_init -> %d (OK=%d)\n", (int)ret, (int)ERR_OK);
  if (ret == ERR_OK) {
    ret = ip_napt_enable_no(SOFTAP_IF, 1);
    Serial.printf("[NAPT] ip_napt_enable_no(SOFTAP_IF) -> %d\n", (int)ret);
  }
  naptStarted = (ret == ERR_OK);
  if (naptStarted) {
    Serial.println(F("[NAPT] Internet forwarding is ACTIVE."));
  } else {
    Serial.println(F("[NAPT] FAILED to enable NAT -- check lwIP Variant build setting!"));
  }
}

// =======================================================================
// Reconnect / watchdog logic
// =======================================================================
void handleStaSupervision() {
  unsigned long now = millis();
  if (now - lastStaCheckMs < STA_CHECK_INTERVAL_MS) return;
  lastStaCheckMs = now;

  bool connected = (WiFi.status() == WL_CONNECTED);

  if (connected) {
    if (!staWasConnected) {
      Serial.printf("[STA] Connected. IP=%s RSSI=%ddBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
    staWasConnected = true;
    staDownSinceMs = 0;
    startNaptIfReady();
    return;
  }

  // Not connected right now.
  if (staWasConnected) {
    // Just dropped.
    Serial.println(F("[STA] Link lost. Attempting reconnect..."));
    staWasConnected = false;
    staDownSinceMs = now;
  }
  if (staDownSinceMs == 0) staDownSinceMs = now;

  WiFi.reconnect(); // cheap retry using stored credentials

  unsigned long downFor = now - staDownSinceMs;
  Serial.printf("[STA] Still down for %lus\n", downFor / 1000);

  if (downFor >= STA_MAX_DOWNTIME_MS) {
    Serial.println(F("[STA] Down too long -- restarting device to recover."));
    delay(200);
    ESP.restart();
  }
}

// =======================================================================
// Web UI
//
// All HTML / CSS / JS lives in flash (PROGMEM) and is streamed to the
// browser in chunks, so a full page is never built in RAM (the ESP8266
// only has ~30-50 KB of free heap). The status page is a static shell:
// it polls /api/status every few seconds and updates in place, so there is
// no full-page reload flicker and the traffic per refresh is ~300 bytes.
// =======================================================================

String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in.charAt(i);
    switch (c) {
      case '&':  out += F("&amp;");  break;
      case '"':  out += F("&quot;"); break;
      case '\'': out += F("&#39;");  break;
      case '<':  out += F("&lt;");   break;
      case '>':  out += F("&gt;");   break;
      default:   out += c;
    }
  }
  return out;
}

String jsonEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in.charAt(i);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((uint8_t)c < 0x20) {
      char b[8];
      snprintf(b, sizeof(b), "\\u%04x", (unsigned)(uint8_t)c);
      out += b;
    } else {
      out += c;
    }
  }
  return out;
}

// ---- Shared page pieces (flash) --------------------------------------

static const char PAGE_HEAD_A[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="dark light">
<title>)HTML";

static const char PAGE_HEAD_B[] PROGMEM = R"HTML(</title>
<style>
:root{--bg:#0b0f14;--surface:#141a22;--surface2:#1b232d;--border:#26313d;--text:#e8edf3;--muted:#8b98a8;
--accent:#3b82f6;--ok:#22c55e;--warn:#f59e0b;--bad:#ef4444;
--ok-bg:rgba(34,197,94,.14);--warn-bg:rgba(245,158,11,.14);--bad-bg:rgba(239,68,68,.14)}
@media (prefers-color-scheme:light){:root{--bg:#f3f5f8;--surface:#fff;--surface2:#eef1f5;--border:#dde2e9;
--text:#131a22;--muted:#5b6776;--accent:#2563eb;--ok:#15803d;--warn:#b45309;--bad:#dc2626;
--ok-bg:rgba(21,128,61,.12);--warn-bg:rgba(180,83,9,.12);--bad-bg:rgba(220,38,38,.10)}}
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;-webkit-font-smoothing:antialiased}
.wrap{max-width:640px;margin:0 auto;padding:0 16px}
.top{background:var(--surface);border-bottom:1px solid var(--border);position:sticky;top:0;z-index:5}
.top .wrap{display:flex;align-items:center;justify-content:space-between;height:56px}
.brand{display:flex;align-items:center;gap:10px;color:var(--text);text-decoration:none;font-weight:650;font-size:16px}
.brand svg{color:var(--accent)}
nav{display:flex;gap:4px}
nav a{color:var(--muted);text-decoration:none;padding:6px 12px;border-radius:8px;font-size:14px;font-weight:500}
nav a:hover,nav a.active{color:var(--text);background:var(--surface2)}
main{padding-top:24px;padding-bottom:24px}
.head{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:18px;flex-wrap:wrap}
h1{font-size:22px;margin:0;font-weight:650;letter-spacing:-.2px}
h2{font-size:12px;margin:0 0 6px;font-weight:600;text-transform:uppercase;letter-spacing:.07em;color:var(--muted)}
.sub{color:var(--muted);margin:4px 0 0;font-size:14px}
.card{background:var(--surface);border:1px solid var(--border);border-radius:12px;padding:16px 18px;margin-bottom:16px}
.pill{display:inline-flex;align-items:center;gap:8px;padding:5px 12px;border-radius:999px;font-size:13px;font-weight:600;background:var(--surface2);color:var(--muted)}
.pill .dot{width:8px;height:8px;border-radius:50%;background:currentColor}
.pill.ok{color:var(--ok);background:var(--ok-bg)}
.pill.warn{color:var(--warn);background:var(--warn-bg)}
.pill.bad{color:var(--bad);background:var(--bad-bg)}
.tiles{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin-bottom:16px}
.tile{background:var(--surface);border:1px solid var(--border);border-radius:12px;padding:14px 16px;min-width:0}
.tile-label{font-size:12px;color:var(--muted);text-transform:uppercase;letter-spacing:.07em}
.tile-value{font-size:24px;font-weight:650;margin-top:4px;font-variant-numeric:tabular-nums;white-space:nowrap}
.unit{font-size:13px;font-weight:500;color:var(--muted)}
.tile-sub{font-size:12.5px;color:var(--muted);margin-top:2px;display:flex;align-items:center;gap:8px;min-height:18px}
.bars{display:inline-flex;align-items:flex-end;gap:2px;height:14px}
.bars i{width:4px;background:var(--border);border-radius:1px}
.bars i:nth-child(1){height:4px}.bars i:nth-child(2){height:7px}.bars i:nth-child(3){height:10px}.bars i:nth-child(4){height:14px}
.bars i.on{background:var(--accent)}
.row{display:flex;justify-content:space-between;gap:16px;padding:11px 0;border-top:1px solid var(--border);font-size:14.5px}
h2+.row{border-top:0}
.row>span:first-child{color:var(--muted);flex:none}
.row>span:last-child{text-align:right;font-weight:500;word-break:break-word}
.v-ok{color:var(--ok)}.v-bad{color:var(--bad)}
.actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:4px}
.btn{display:inline-flex;align-items:center;justify-content:center;padding:10px 18px;border-radius:8px;border:1px solid var(--border);background:var(--surface2);color:var(--text);font:inherit;font-size:14px;font-weight:600;text-decoration:none;cursor:pointer}
.btn:hover{border-color:var(--muted)}
.btn.primary{background:var(--accent);border-color:var(--accent);color:#fff}
.btn.primary:hover{filter:brightness(1.1)}
.meta{color:var(--muted);font-size:12.5px;margin:16px 0 0}
label{display:block;margin:16px 0 6px;font-size:13.5px;font-weight:600}
h2+label{margin-top:10px}
input,select{width:100%;padding:10px 12px;background:var(--bg);border:1px solid var(--border);border-radius:8px;color:var(--text);font:inherit}
input:focus,select:focus{outline:2px solid var(--accent);outline-offset:-1px;border-color:var(--accent)}
small{display:block;margin-top:6px;color:var(--muted);font-size:12.5px}
.pw{position:relative}
.pw input{padding-right:70px}
.pw button{position:absolute;right:6px;top:50%;transform:translateY(-50%);background:none;border:0;color:var(--accent);font:inherit;font-size:13px;font-weight:600;cursor:pointer;padding:6px 10px}
.center{text-align:center;padding:36px 18px}
.spinner{width:36px;height:36px;border:3px solid var(--border);border-top-color:var(--accent);border-radius:50%;animation:spin 1s linear infinite;margin:0 auto 18px}
@keyframes spin{to{transform:rotate(360deg)}}
.notice{border-left:4px solid var(--bad)}
footer{text-align:center;color:var(--muted);font-size:12.5px;padding:0 16px 28px}
@media (max-width:420px){.tile{padding:12px}.tile-value{font-size:20px}}
</style></head><body>
<header class="top"><div class="wrap">
<a class="brand" href="/"><svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><circle cx="12" cy="20" r="1" fill="currentColor"/></svg><span>Wifi Repeater</span></a>
<nav>)HTML";

static const char PAGE_HEAD_C[] PROGMEM = R"HTML(</nav></div></header>
<main class="wrap">
)HTML";

static const char PAGE_FOOT[] PROGMEM = R"HTML(
</main>
<footer>Wifi Repeater &middot; NAT router mode</footer>
</body></html>)HTML";

// Starts a chunked (streamed) HTML response and sends the common header.
void beginPage(int code, const String &title, bool statusActive) {
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(code, "text/html", "");
  server.sendContent_P(PAGE_HEAD_A);
  server.sendContent(title);
  server.sendContent(F(" &middot; Wifi Repeater"));
  server.sendContent_P(PAGE_HEAD_B);
  if (statusActive) {
    server.sendContent(F("<a href=\"/\" class=\"active\">Status</a><a href=\"/config\">Settings</a>"));
  } else {
    server.sendContent(F("<a href=\"/\">Status</a><a href=\"/config\" class=\"active\">Settings</a>"));
  }
  server.sendContent_P(PAGE_HEAD_C);
}

// Sends the footer and terminates the chunked response.
void endPage() {
  server.sendContent_P(PAGE_FOOT);
  server.sendContent("");
}

// Simple message page (errors, 404).
void sendNoticePage(int code, const String &heading, const String &message) {
  beginPage(code, heading, true);
  server.sendContent(F("<div class=\"card notice\"><h1>"));
  server.sendContent(htmlEscape(heading));
  server.sendContent(F("</h1><p class=\"sub\">"));
  server.sendContent(htmlEscape(message));
  server.sendContent(F("</p><p class=\"actions\"><a class=\"btn\" href=\"/config\">Back to settings</a>"
                       "<a class=\"btn\" href=\"/\">Status</a></p></div>"));
  endPage();
}

// "Restarting" page: shows a spinner, then jumps back to the status page
// as soon as the device answers again.
static const char RESTART_TAIL[] PROGMEM = R"HTML(</p>
<p class="sub">If this page does not reload on its own, reconnect to the repeater's Wi-Fi network and <a href="/" style="color:var(--accent)">open the status page</a>.</p>
</div>
<script>
setTimeout(function p(){
  fetch("/api/status",{cache:"no-store"})
    .then(function(r){if(r.ok){location.href="/"}else{throw 0}})
    .catch(function(){setTimeout(p,2000)});
},6000);
</script>
)HTML";

void sendRestartPage(const String &heading, const String &message) {
  beginPage(200, heading, true);
  server.sendContent(F("<div class=\"card center\"><div class=\"spinner\"></div><h1>"));
  server.sendContent(heading);
  server.sendContent(F("</h1><p class=\"sub\">"));
  server.sendContent(message);
  server.sendContent_P(RESTART_TAIL);
  endPage();
}

// ---- Status page (static shell + live JSON) ------------------------------

static const char STATUS_BODY[] PROGMEM = R"HTML(
<div class="head">
  <div><h1>Status</h1><p class="sub" id="summary">Loading&hellip;</p></div>
  <span class="pill" id="pill"><span class="dot"></span><span id="pillText">Checking</span></span>
</div>

<div class="tiles">
  <div class="tile">
    <div class="tile-label">Signal</div>
    <div class="tile-value"><span id="rssi">&ndash;</span> <span class="unit">dBm</span></div>
    <div class="tile-sub"><span class="bars" id="bars"><i></i><i></i><i></i><i></i></span><span id="rssiQ">&ndash;</span></div>
  </div>
  <div class="tile">
    <div class="tile-label">Devices</div>
    <div class="tile-value" id="clients">&ndash;</div>
    <div class="tile-sub" id="clientsSub">connected</div>
  </div>
  <div class="tile">
    <div class="tile-label">Uptime</div>
    <div class="tile-value" id="uptime">&ndash;</div>
    <div class="tile-sub">since last boot</div>
  </div>
</div>

<div class="card">
  <h2>Upstream connection</h2>
  <div class="row"><span>Status</span><span id="vSta">&ndash;</span></div>
  <div class="row"><span>Router network</span><span id="vRouter">&ndash;</span></div>
  <div class="row"><span>IP address</span><span id="vStaIp">&ndash;</span></div>
  <div class="row"><span>Wi-Fi channel</span><span id="vCh">&ndash;</span></div>
</div>

<div class="card">
  <h2>Repeater network</h2>
  <div class="row"><span>Network name</span><span id="vAp">&ndash;</span></div>
  <div class="row"><span>Gateway address</span><span id="vApIp">&ndash;</span></div>
  <div class="row"><span>Security</span><span>WPA2-PSK</span></div>
  <div class="row"><span>Internet forwarding (NAT)</span><span id="vNat">&ndash;</span></div>
</div>

<div class="actions">
  <a class="btn primary" href="/config">Settings</a>
  <a class="btn" href="/restart" onclick="return confirm('Restart the repeater now?')">Restart</a>
</div>
<p class="meta">Free memory <span id="heap">&ndash;</span> &middot; updates every 3 seconds</p>

<script>
(function(){
  var $=function(i){return document.getElementById(i)};
  function txt(id,t,cls){var e=$(id);e.textContent=t;if(cls!==undefined)e.className=cls}
  function uptime(s){
    var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);
    if(d)return d+"d "+h+"h";
    if(h)return h+"h "+m+"m";
    if(m)return m+"m "+(s%60)+"s";
    return s+"s";
  }
  function quality(r){return r>=-55?["Excellent",4]:r>=-65?["Good",3]:r>=-75?["Fair",2]:["Weak",1]}
  function bars(n){var b=$("bars").children;for(var i=0;i<b.length;i++)b[i].className=i<n?"on":""}
  function render(d){
    var online=d.sta&&d.nat;
    $("pill").className="pill "+(online?"ok":d.sta?"warn":"bad");
    txt("pillText",online?"Online":d.sta?"NAT inactive":"Upstream offline");
    txt("summary",d.sta?"Relaying \u201C"+d.routerSsid+"\u201D as \u201C"+d.apSsid+"\u201D":"Waiting for connection to \u201C"+d.routerSsid+"\u201D");
    if(d.sta){var q=quality(d.rssi);txt("rssi",d.rssi);txt("rssiQ",q[0]);bars(q[1])}
    else{txt("rssi","\u2013");txt("rssiQ","No signal");bars(0)}
    txt("clients",d.clients);
    txt("clientsSub","of "+d.maxClients+" max");
    txt("uptime",uptime(d.uptime));
    txt("vSta",d.sta?"Connected":"Disconnected",d.sta?"v-ok":"v-bad");
    txt("vRouter",d.routerSsid);
    txt("vStaIp",d.sta?d.staIp:"\u2013");
    txt("vCh",d.channel);
    txt("vAp",d.apSsid);
    txt("vApIp",d.apIp);
    txt("vNat",d.nat?"Active":"Inactive",d.nat?"v-ok":"v-bad");
    txt("heap",(d.heap/1024).toFixed(1)+" KB");
  }
  function fail(){
    $("pill").className="pill bad";
    txt("pillText","Unreachable");
    txt("summary","Can\u2019t reach the repeater \u2013 retrying\u2026");
  }
  function tick(){
    fetch("/api/status",{cache:"no-store"})
      .then(function(r){return r.json()})
      .then(render)
      .catch(fail)
      .then(function(){setTimeout(tick,3000)});
  }
  tick();
})();
</script>
)HTML";

// ---- Settings page ---------------------------------------------------------

static const char CONFIG_A[] PROGMEM = R"HTML(
<div class="head"><div><h1>Settings</h1><p class="sub">Changes take effect after the repeater restarts.</p></div></div>
<form method="POST" action="/save">
  <div class="card">
    <h2>Upstream router</h2>
    <label for="rs">Network name (SSID)</label>
    <input id="rs" name="router_ssid" maxlength="32" autocomplete="off" required value=")HTML";

static const char CONFIG_B[] PROGMEM = R"HTML(">
    <label for="rp">Password</label>
    <div class="pw"><input id="rp" name="router_pass" type="password" minlength="8" maxlength="64" autocomplete="new-password" placeholder="Unchanged"><button type="button">Show</button></div>
    <small>The Wi-Fi network this repeater connects to. Leave blank to keep the current password.</small>
  </div>

  <div class="card">
    <h2>Repeater network</h2>
    <label for="as">Network name (SSID)</label>
    <input id="as" name="ap_ssid" maxlength="32" autocomplete="off" required value=")HTML";

static const char CONFIG_C[] PROGMEM = R"HTML(">
    <label for="ap">Password</label>
    <div class="pw"><input id="ap" name="ap_pass" type="password" minlength="8" maxlength="64" autocomplete="new-password" placeholder="Unchanged"><button type="button">Show</button></div>
    <small>At least 8 characters (WPA2). Leave blank to keep the current password.</small>
    <label for="ac">Wi-Fi channel</label>
    <select id="ac" name="ap_channel">)HTML";

static const char CONFIG_D[] PROGMEM = R"HTML(</select>
    <small>While connected to the router, the repeater automatically follows the router's channel.</small>
  </div>

  <div class="actions">
    <button class="btn primary" type="submit">Save &amp; restart</button>
    <a class="btn" href="/">Cancel</a>
  </div>
</form>
<script>
document.querySelectorAll(".pw button").forEach(function(b){
  b.onclick=function(){
    var i=b.previousElementSibling,show=i.type==="password";
    i.type=show?"text":"password";
    b.textContent=show?"Hide":"Show";
  };
});
</script>
)HTML";

// =======================================================================
// Web handlers
// =======================================================================
void handleRoot() {
  beginPage(200, F("Status"), true);
  server.sendContent_P(STATUS_BODY);
  endPage();
}

void handleApiStatus() {
  bool staUp = (WiFi.status() == WL_CONNECTED);
  String j;
  j.reserve(360);
  j += F("{\"sta\":");           j += staUp ? F("true") : F("false");
  j += F(",\"nat\":");           j += naptStarted ? F("true") : F("false");
  j += F(",\"routerSsid\":\"");  j += jsonEscape(cfg.routerSsid);
  j += F("\",\"apSsid\":\"");    j += jsonEscape(cfg.apSsid);
  j += F("\",\"staIp\":\"");     j += staUp ? WiFi.localIP().toString() : String("");
  j += F("\",\"apIp\":\"");      j += WiFi.softAPIP().toString();
  j += F("\",\"rssi\":");        j += String(staUp ? WiFi.RSSI() : 0);
  j += F(",\"channel\":");       j += String(WiFi.channel());
  j += F(",\"clients\":");       j += String(WiFi.softAPgetStationNum());
  j += F(",\"maxClients\":");    j += String(MAX_AP_CLIENTS);
  j += F(",\"uptime\":");        j += String(millis() / 1000);
  j += F(",\"heap\":");          j += String(ESP.getFreeHeap());
  j += '}';
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

void handleConfigForm() {
  beginPage(200, F("Settings"), false);
  server.sendContent_P(CONFIG_A);
  server.sendContent(htmlEscape(cfg.routerSsid));
  server.sendContent_P(CONFIG_B);
  server.sendContent(htmlEscape(cfg.apSsid));
  server.sendContent_P(CONFIG_C);
  for (int ch = 1; ch <= 13; ch++) {
    String o = F("<option value=\"");
    o += String(ch);
    o += '"';
    if (ch == cfg.apChannel) o += F(" selected");
    o += '>';
    o += String(ch);
    o += F("</option>");
    server.sendContent(o);
  }
  server.sendContent_P(CONFIG_D);
  endPage();
}

void handleSave() {
  String routerSsid = server.arg("router_ssid");
  String routerPass = server.arg("router_pass");
  String apSsid     = server.arg("ap_ssid");
  String apPass     = server.arg("ap_pass");
  String apChanStr  = server.arg("ap_channel");

  if (routerSsid.length() == 0 || apSsid.length() == 0) {
    sendNoticePage(400, F("Can't save settings"), F("Network names (SSIDs) cannot be empty."));
    return;
  }
  if (routerPass.length() > 0 && routerPass.length() < 8) {
    sendNoticePage(400, F("Can't save settings"), F("The router password must be at least 8 characters."));
    return;
  }
  if (apPass.length() > 0 && apPass.length() < 8) {
    sendNoticePage(400, F("Can't save settings"), F("The repeater password must be at least 8 characters (WPA2 minimum)."));
    return;
  }

  strlcpy(cfg.routerSsid, routerSsid.c_str(), sizeof(cfg.routerSsid));
  if (routerPass.length() > 0) {
    strlcpy(cfg.routerPass, routerPass.c_str(), sizeof(cfg.routerPass));
  }
  strlcpy(cfg.apSsid, apSsid.c_str(), sizeof(cfg.apSsid));
  if (apPass.length() > 0) {
    strlcpy(cfg.apPass, apPass.c_str(), sizeof(cfg.apPass));
  }
  int ch = apChanStr.toInt();
  if (ch >= 1 && ch <= 13) cfg.apChannel = (uint8_t)ch;

  saveConfig();

  sendRestartPage(F("Settings saved"), F("Restarting the repeater to apply your changes\u2026"));
  delay(400);
  ESP.restart();
}

void handleRestart() {
  sendRestartPage(F("Restarting"), F("The repeater is rebooting and will be back in a few seconds."));
  delay(300);
  ESP.restart();
}

void handleNotFound() {
  sendNoticePage(404, F("Page not found"), F("The page you requested doesn't exist."));
}

// =======================================================================
// setup / loop
// =======================================================================
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n\nWifi Repeater booting..."));

  loadConfig();

  WiFi.mode(WIFI_AP_STA);

  // Disabling modem sleep is the single biggest latency/jitter win on the
  // ESP8266 -- without this, the radio periodically naps to save power,
  // which adds tens of ms of jitter that hurts VoIP calls noticeably.
  WiFi.setSleepMode(WIFI_NONE_SLEEP);

  // Reconnect automatically at the driver level too (belt-and-suspenders
  // alongside the manual supervision loop below).
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false); // we manage credentials ourselves via EEPROM

  startAP();
  startStation();

  MDNS.begin("repeater"); // reachable at http://repeater.local/

  server.on("/", handleRoot);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/favicon.ico", []() { server.send(204); });
  server.on("/config", HTTP_GET, handleConfigForm);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/restart", handleRestart);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println(F("Web config server started on port 80."));
}

void loop() {
  server.handleClient();
  MDNS.update();
  handleStaSupervision();
}
