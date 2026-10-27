// G0D SZIP — very lightweight built-in GUI.
// A tiny HTTP server (no dependencies: one file, plain sockets) that serves a
// single embedded page and runs the same in-tree encoder/decoder in-process.
//   g0dszip g [port]      ->  http://localhost:8848
#include "szip.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using sock_t = SOCKET;
static void sock_close(sock_t s) { closesocket(s); }
static int sock_send(sock_t s, const char *p, int n) {
  return send(s, p, n, 0);
}
static void sock_init() {
  WSADATA w;
  WSAStartup(MAKEWORD(2, 2), &w);
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static void sock_close(sock_t s) { close(s); }
static int sock_send(sock_t s, const char *p, int n) {
  return (int)send(s, p, (size_t)n, MSG_NOSIGNAL);
}
static void sock_init() { signal(SIGPIPE, SIG_IGN); }
#endif

namespace szip {
namespace {

// ---------------------------------------------------------------- the page --
const char kPage[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>G0D SZIP</title>
<style>
:root{--bg:#0f1216;--card:#171c22;--acc:#4fd1c5;--txt:#e6edf3;--dim:#8b98a5}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--txt);
 font:15px/1.5 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;
 display:flex;flex-direction:column;align-items:center;min-height:100vh}
h1{margin:28px 0 2px;font-size:26px;letter-spacing:.5px}
h1 b{color:var(--acc)}
.sub{color:var(--dim);margin:0 0 22px;font-size:13px}
.card{background:var(--card);border:1px solid #232a33;border-radius:14px;
 padding:22px;width:min(560px,92vw);margin-bottom:16px}
#drop{border:2px dashed #2c3542;border-radius:12px;padding:38px 18px;
 text-align:center;color:var(--dim);cursor:pointer;transition:.15s}
#drop.hot{border-color:var(--acc);color:var(--txt);background:#14202a}
#drop b{color:var(--acc)}
.row{display:flex;gap:10px;margin-top:16px;flex-wrap:wrap}
button{flex:1;min-width:130px;padding:11px 14px;border:0;border-radius:9px;
 font-size:14px;font-weight:600;cursor:pointer;background:#232a33;color:var(--txt)}
button:disabled{opacity:.45;cursor:default}
button.primary{background:var(--acc);color:#08222b}
button.ghost{background:transparent;border:1px solid #2c3542}
.toggle{display:flex;align-items:center;gap:9px;margin-top:14px;color:var(--dim);font-size:13px}
.toggle input{accent-color:var(--acc);width:16px;height:16px}
#stat{margin-top:14px;font-size:13px;color:var(--dim);min-height:20px;white-space:pre-line}
#stat.ok{color:var(--acc)}#stat.err{color:#f28b82}
.spin{display:inline-block;width:13px;height:13px;border:2px solid var(--acc);
 border-top-color:transparent;border-radius:50%;animation:sp .7s linear infinite;
 vertical-align:-2px;margin-right:7px}
@keyframes sp{to{transform:rotate(360deg)}}
.foot{color:#5b6672;font-size:12px;margin:6px 0 26px}
a{color:var(--acc)}
</style></head><body>
<h1>G0D <b>SZIP</b></h1>
<p class="sub">own encoder &middot; own decoder &middot; .szip format v4</p>
<div class="card">
  <div id="drop">drop a file here, or <b>browse</b><br>
    <input id="file" type="file" style="display:none"></div>
  <div class="toggle"><input id="fast" type="checkbox">
    <label for="fast">Fast parse &mdash; quicker, slightly larger output</label></div>
  <div class="row">
    <button id="cbtn" class="primary" disabled>Compress &rarr; .szip</button>
    <button id="dbtn" disabled>Decompress</button>
    <button id="dl" class="ghost" disabled>Download result</button>
  </div>
  <div id="stat"></div>
</div>
<p class="foot">runs 100% in this process &mdash; nothing leaves your machine</p>
<script>
const $=id=>document.getElementById(id);
let f=null,out=null,outName='';
const stat=(t,cls)=>{const s=$('stat');s.className=cls||'';s.textContent=t;};
const ready=()=>{$('cbtn').disabled=!f;$('dbtn').disabled=!f;
  $('dl').disabled=!out;};
$('drop').onclick=()=>$('file').click();
$('file').onchange=e=>{f=e.target.files[0];if(f){out=null;
  $('drop').innerHTML='<b>'+f.name+'</b> — '+(f.size<1024?f.size+' B':
  (f.size/1048576).toFixed(2)+' MB');stat('');ready();}};
['dragover','dragenter'].forEach(ev=>$('drop').addEventListener(ev,e=>{
  e.preventDefault();$('drop').classList.add('hot');}));
['dragleave','drop'].forEach(ev=>$('drop').addEventListener(ev,e=>{
  e.preventDefault();$('drop').classList.remove('hot');}));
$('drop').addEventListener('drop',e=>{f=e.dataTransfer.files[0];
  if(f){out=null;$('drop').innerHTML='<b>'+f.name+'</b> — '+(f.size<1024?
  f.size+' B':(f.size/1048576).toFixed(2)+' MB');stat('');ready();}});

async function run(path,label){
  if(!f)return;
  $('cbtn').disabled=$('dbtn').disabled=true;
  stat('','');$('stat').innerHTML='<span class="spin"></span>'+label+'…';
  const t0=performance.now();
  try{
    const r=await fetch(path,{method:'POST',body:f});
    if(!r.ok)throw new Error('server error '+r.status);
    out=await r.blob();
    const dt=((performance.now()-t0)/1000).toFixed(2);
    const a=+r.headers.get('X-Original-Size')||f.size;
    const b=+r.headers.get('X-Result-Size')||out.size;
    const pct=a?((100*b)/a).toFixed(2):'—';
    outName=f.name+(path.endsWith('/c')?'.szip':'.out');
    stat(f.name+': '+a+' B → '+b+' B  ('+pct+'% of input)  in '+dt+' s','ok');
    $('dl').disabled=false;
  }catch(e){stat('failed: '+e.message,'err');out=null;}
  ready();
}
$('cbtn').onclick=()=>run('api/c'+($('fast').checked?'?fast=1':''),'compressing');
$('dbtn').onclick=()=>run('api/d','decompressing');
$('dl').onclick=()=>{if(!out)return;const u=URL.createObjectURL(out);
  const a=document.createElement('a');a.href=u;a.download=outName;a.click();
  URL.revokeObjectURL(u);};
</script></body></html>)HTML";

// ----------------------------------------------------------------- server --
constexpr size_t kMaxHead = 64 * 1024;
constexpr size_t kMaxBody = (size_t)256 << 20;

bool send_all(sock_t s, const std::string &hdr, const char *body, size_t n) {
  size_t off = 0;
  while (off < hdr.size()) {
    int k = sock_send(s, hdr.data() + off, (int)(hdr.size() - off));
    if (k <= 0) return false;
    off += (size_t)k;
  }
  off = 0;
  while (off < n) {
    int k = sock_send(s, body + off, (int)std::min<size_t>(n - off, 1 << 20));
    if (k <= 0) return false;
    off += (size_t)k;
  }
  return true;
}

void respond(sock_t s, int code, const char *type,
             const std::string &extra, const char *body, size_t n) {
  const char *msg = code == 200 ? "OK" : code == 204 ? "No Content" : "Error";
  std::string h = "HTTP/1.1 " + std::to_string(code) + " " + msg +
                  "\r\nContent-Type: " + type +
                  "\r\nContent-Length: " + std::to_string(n) +
                  "\r\nAccess-Control-Allow-Origin: *" +
                  "\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS" +
                  "\r\nAccess-Control-Allow-Headers: *" +
                  "\r\nConnection: close\r\n" + extra + "\r\n";
  send_all(s, h, body, n);
}

void handle_client(sock_t s) {
  // read head
  std::string head;
  char buf[4096];
  size_t body_off = 0;
  std::string pending; // bytes after the head within the same reads
  while (head.find("\r\n\r\n") == std::string::npos) {
    int k = (int)recv(s, buf, (int)sizeof(buf), 0);
    if (k <= 0) { sock_close(s); return; }
    head.append(buf, (size_t)k);
    if (head.size() > kMaxHead) { sock_close(s); return; }
  }
  body_off = head.find("\r\n\r\n") + 4;
  pending = head.substr(body_off);

  // request line
  const size_t sp1 = head.find(' ');
  const size_t sp2 = head.find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos) {
    respond(s, 400, "text/plain", "", "bad request", 12);
    sock_close(s);
    return;
  }
  const std::string method = head.substr(0, sp1);
  std::string path = head.substr(sp1 + 1, sp2 - sp1 - 1);

  if (method == "OPTIONS") {
    respond(s, 204, "text/plain", "", "", 0);
    sock_close(s);
    return;
  }

  // content-length
  size_t clen = 0;
  {
    std::string lower = head;
    for (auto &c : lower) c = (char)tolower((unsigned char)c);
    const size_t p = lower.find("content-length:");
    if (p != std::string::npos) clen = (size_t)strtoull(lower.c_str() + p + 15, nullptr, 10);
  }

  if (method == "GET" && (path == "/" || path == "/index.html")) {
    respond(s, 200, "text/html; charset=utf-8", "", kPage, sizeof(kPage) - 1);
    sock_close(s);
    return;
  }

  const bool is_c = path.rfind("/api/c", 0) == 0;
  const bool is_d = path.rfind("/api/d", 0) == 0;
  if ((method != "POST") || (!is_c && !is_d) || clen > kMaxBody) {
    respond(s, 404, "text/plain", "", "not found", 10);
    sock_close(s);
    return;
  }

  // read body
  std::vector<uint8_t> body;
  body.reserve(clen + 1);
  body.assign(pending.begin(), pending.end());
  while (body.size() < clen) {
    int k = (int)recv(s, buf, (int)sizeof(buf), 0);
    if (k <= 0) { sock_close(s); return; }
    body.insert(body.end(), buf, buf + k);
    if (body.size() > kMaxBody) { sock_close(s); return; }
  }

  const int fast = path.find("fast=1") != std::string::npos;
  std::vector<uint8_t> out;
  int e;
  if (is_c) {
    set_parse_fast(fast);
    e = compress_buffer(body.data(), body.size(), out);
    set_parse_fast(0);
  } else {
    e = decompress_buffer(body.data(), body.size(), out);
  }
  if (e != SZIP_OK) {
    const std::string msg = std::string("codec error ") + std::to_string(e);
    respond(s, 400, "text/plain", "", msg.data(), msg.size());
    sock_close(s);
    return;
  }
  const std::string extra =
      "X-Original-Size: " + std::to_string(body.size()) + "\r\n" +
      "X-Result-Size: " + std::to_string(out.size()) + "\r\n" +
      "Content-Disposition: attachment\r\n";
  respond(s, 200, "application/octet-stream", extra,
          (const char *)out.data(), out.size());
  sock_close(s);
}

} // namespace

int run_gui(unsigned short port) {
  sock_init();
  sock_t l = socket(AF_INET, SOCK_STREAM, 0);
  if (l == (sock_t)-1) {
    std::fprintf(stderr, "gui: cannot create socket\n");
    return 1;
  }
  int one = 1;
  setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
  sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);
  if (bind(l, (sockaddr *)&addr, sizeof(addr)) < 0) {
    std::fprintf(stderr, "gui: cannot bind port %u\n", (unsigned)port);
    return 1;
  }
  listen(l, 16);
  std::printf("G0D SZIP GUI: http://localhost:%u  (Ctrl+C to stop)\n",
              (unsigned)port);
  std::fflush(stdout);
  for (;;) {
    sockaddr_in ca;
    socklen_t cl = sizeof(ca);
    sock_t c = accept(l, (sockaddr *)&ca, &cl);
    if (c < 0) continue;
    std::thread(handle_client, c).detach();
  }
}

} // namespace szip
