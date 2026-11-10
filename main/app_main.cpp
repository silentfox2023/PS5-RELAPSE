/*
 * PS5 Relapse 随身分发主机（方案 A）
 * ESP32-S3 起一个 WPA2 热点 + HTTP 服务器（8000 端口）：
 *  - 从 LittleFS 分区分发 Relapse-Exploit 的网页与 payload（等价 serve.py）
 *  - /manage 管理页：上传自定义 payload、列出/删除、一键 TCP 推送到 PS5 的 elfldr(9021)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

static const char *TAG = "relapse-stick";

#define FS_BASE      "/www"
#define CUSTOM_DIR   FS_BASE "/payloads_custom"
#define HTTP_PORT    8000
#define ELFLDR_PORT  9021
#define SEND_CHUNK   4096
#define MAX_UPLOAD   (1024 * 1024) /* 单个自定义 payload 上限 1MB */

static char g_ps5_ip[16] = ""; /* 最近一次访问 exploit 入口页的设备 IP */

/* ---------- 小工具 ---------- */

static bool valid_name(const char *n)
{
    size_t l = strlen(n);
    if (l == 0 || l > 63) return false;
    if (n[0] == '.') return false;
    for (const char *p = n; *p; p++) {
        char c = *p;
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

static bool valid_ip(const char *s)
{
    struct in_addr a;
    return inet_pton(AF_INET, s, &a) == 1;
}

static bool get_query(httpd_req_t *req, const char *key, char *val, size_t vlen)
{
    char q[192];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    return httpd_query_key_value(q, key, val, vlen) == ESP_OK;
}

static void custom_path(const char *name, char *out, size_t outlen)
{
    snprintf(out, outlen, CUSTOM_DIR "/%s", name);
}

/* ---------- LittleFS ---------- */

static void mount_littlefs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = FS_BASE,
        .partition_label = "storage",
        .partition = NULL,
        .format_if_mount_failed = true,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&conf));

    size_t total = 0, used = 0;
    if (esp_littlefs_info("storage", &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "littlefs mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
    }
    mkdir(CUSTOM_DIR, 0775); /* 自定义 payload 目录，已存在则忽略 */
}

/* ---------- WiFi AP ---------- */

static void wifi_start_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wc = {};
    strncpy((char *)wc.ap.ssid, CONFIG_APP_SSID, sizeof(wc.ap.ssid) - 1);
    wc.ap.ssid_len = strlen(CONFIG_APP_SSID);
    strncpy((char *)wc.ap.password, CONFIG_APP_PASS, sizeof(wc.ap.password) - 1);
    wc.ap.channel = 1;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.max_connection = 4;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP up: SSID=\"%s\" channel=%d", CONFIG_APP_SSID, wc.ap.channel);
}

/* ---------- 静态分发（serve.py 等价物） ---------- */

static const char *mime_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL) return "application/octet-stream";
    if (strcasecmp(dot, ".html") == 0) return "text/html";
    if (strcasecmp(dot, ".htm") == 0)  return "text/html";
    if (strcasecmp(dot, ".js") == 0)   return "application/javascript";
    if (strcasecmp(dot, ".json") == 0) return "application/json";
    if (strcasecmp(dot, ".css") == 0)  return "text/css";
    if (strcasecmp(dot, ".png") == 0)  return "image/png";
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcasecmp(dot, ".svg") == 0)  return "image/svg+xml";
    if (strcasecmp(dot, ".md") == 0 || strcasecmp(dot, ".txt") == 0) return "text/plain";
    if (strcasecmp(dot, ".elf") == 0)  return "application/octet-stream";
    return "application/octet-stream";
}

static esp_err_t static_get_handler(httpd_req_t *req)
{
    char uri[256];
    strncpy(uri, req->uri, sizeof(uri) - 1);
    uri[sizeof(uri) - 1] = '\0';

    char *q = strchr(uri, '?');
    if (q != NULL) *q = '\0';

    if (uri[0] == '\0' || strcmp(uri, "/") == 0) strcpy(uri, "/index.html");
    if (strstr(uri, "..") != NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }

    /* 谁打开了 exploit 入口页，就把它记为 PS5 */
    if (strcmp(uri, "/index.html") == 0) {
        int fd = httpd_req_to_sockfd(req);
        struct sockaddr_in c;
        socklen_t cl = sizeof(c);
        if (fd >= 0 && getpeername(fd, (struct sockaddr *)&c, &cl) == 0 && c.sin_family == AF_INET) {
            inet_ntoa_r(c.sin_addr, g_ps5_ip, sizeof(g_ps5_ip));
            ESP_LOGI(TAG, "exploit page opened by %s", g_ps5_ip);
        }
    }

    char path[sizeof(uri) + sizeof(FS_BASE)];
    snprintf(path, sizeof(path), FS_BASE "%s", uri);

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGW(TAG, "404: %s", uri);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }

    httpd_resp_set_type(req, mime_for(uri));
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");

    char clen[16];
    snprintf(clen, sizeof(clen), "%ld", (long)st.st_size);
    httpd_resp_set_hdr(req, "Content-Length", clen);

    char *buf = (char *)malloc(SEND_CHUNK);
    if (buf == NULL) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    size_t n;
    while ((n = fread(buf, 1, SEND_CHUNK, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
    }
    if (err == ESP_OK && ferror(f)) err = ESP_FAIL;
    if (err == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);

    free(buf);
    fclose(f);
    ESP_LOGI(TAG, "200: %s (%s)", uri, clen);
    return err;
}

/* ---------- 管理页 ---------- */

static const char MANAGE_HTML[] = R"HTML(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Relapse Stick</title><style>
body{background:#0b0b0d;color:#eee;font:15px/1.6 system-ui,"PingFang SC","Microsoft YaHei",sans-serif;padding:16px;max-width:760px;margin:auto}
h1{font-size:18px;margin:4px 0 12px}a{color:#7ee787}
.card{background:#161618;border:1px solid #2a2a2e;border-radius:10px;padding:14px;margin:12px 0}
button{background:#1f6feb;color:#fff;border:0;border-radius:6px;padding:6px 12px;margin-left:6px;cursor:pointer}
button.del{background:#6e2b2b}
input[type=text]{background:#0b0b0d;color:#eee;border:1px solid #2a2a2e;border-radius:6px;padding:6px;width:150px}
table{width:100%;border-collapse:collapse}td,th{text-align:left;padding:6px 4px;border-bottom:1px solid #2a2a2e;font-size:14px}
.mut{color:#8a8a93;font-size:13px}
#msg{white-space:pre-wrap}
</style></head><body>
<h1>PS5 Relapse Stick</h1>
<div class=card>Exploit 入口：<a href=/>/ (index.html)</a><br>
PS5 IP：<b id=ps5>-</b>（打开过 exploit 页的设备自动记录）
<input id=ip type=text placeholder="手动覆盖 PS5 IP">
<span class=mut>剩余空间：<span id=free>-</span></span></div>
<div class=card><b>上传自定义 payload（ELF/bin）</b><br><br>
<input type=file id=file> <button onclick=up()>上传</button></div>
<div class=card><b>自定义 payload 列表</b>
<table><thead><tr><th>文件</th><th>大小</th><th>操作</th></tr></thead>
<tbody id=rows></tbody></table></div>
<div class=card id=msg>就绪。越狱成功、elfldr 在 PS5 上监听 9021 后，点“发送到 PS5”即可推送 payload。</div>
<script>
let ps5='';
function fmt(n){return n>1024?(n/1024).toFixed(1)+' KB':n+' B'}
function log(t){document.getElementById('msg').textContent=t}
async function refresh(){
 try{
  const j=await(await fetch('/api/list')).json();
  ps5=j.ps5||'';
  document.getElementById('ps5').textContent=ps5||'未知';
  document.getElementById('free').textContent=fmt(j.free);
  const tb=document.getElementById('rows');tb.innerHTML='';
  for(const f of j.files){
   const tr=document.createElement('tr');
   tr.innerHTML='<td>'+f.n+'</td><td>'+fmt(f.s)+'</td><td style=text-align:right>'+
    '<button onclick="push(\''+f.n+'\')">发送到 PS5</button>'+
    '<button class=del onclick="del(\''+f.n+'\')">删除</button></td>';
   tb.appendChild(tr)}
  if(!j.files.length)tb.innerHTML='<tr><td colspan=3 class=mut>暂无文件，请先上传</td></tr>';
 }catch(e){log('刷新失败: '+e)}}
async function up(){
 const f=document.getElementById('file').files[0];
 if(!f)return log('请选择文件');
 log('上传 '+f.name+' ('+fmt(f.size)+') ...');
 try{
  const r=await fetch('/upload?name='+encodeURIComponent(f.name),{method:'POST',body:f});
  log(await r.text());refresh()}catch(e){log('上传失败: '+e)}}
async function del(n){
 if(!confirm('删除 '+n+' ?'))return;
 try{const r=await fetch('/delete?name='+encodeURIComponent(n));log(await r.text());refresh()}catch(e){log('删除失败: '+e)}}
async function push(n){
 const ip=document.getElementById('ip').value||ps5;
 if(!ip)return log('PS5 IP 未知，请在上方手动填写');
 log('推送 '+n+' -> '+ip+':9021 ...');
 try{const r=await fetch('/push?name='+encodeURIComponent(n)+'&ip='+ip);log(await r.text())}catch(e){log('推送失败: '+e)}}
refresh();setInterval(refresh,5000);
</script></body></html>)HTML";

static esp_err_t manage_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, MANAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_list_get_handler(httpd_req_t *req)
{
    char *buf = (char *)malloc(4096);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    size_t freeb = total > used ? total - used : 0;

    int cap = 4096;
    int n = snprintf(buf, cap, "{\"ps5\":\"%s\",\"free\":%u,\"files\":[", g_ps5_ip, (unsigned)freeb);
    if (n < 0 || n >= cap) n = cap - 2;

    DIR *d = opendir(CUSTOM_DIR);
    if (d != NULL) {
        struct dirent *e;
        bool first = true;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char path[128];
            custom_path(e->d_name, path, sizeof(path));
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            int w = snprintf(buf + n, cap - n, "%s{\"n\":\"%s\",\"s\":%ld}",
                             first ? "" : ",", e->d_name, (long)st.st_size);
            if (w > 0 && n + w < cap) { n += w; first = false; }
        }
        closedir(d);
    }
    if (n < cap - 3) {
        n += snprintf(buf + n, cap - n, "]}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, buf, n);
    free(buf);
    return err;
}

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    char name[64];
    if (!get_query(req, "name", name, sizeof(name)) || !valid_name(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_FAIL;
    }
    if (req->content_len == 0 || req->content_len > MAX_UPLOAD) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "size 0 or >1MB");
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    if (used + req->content_len > total) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage full");
        return ESP_FAIL;
    }

    char path[128];
    custom_path(name, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_FAIL;
    }

    char *buf = (char *)malloc(SEND_CHUNK);
    if (buf == NULL) {
        fclose(f);
        unlink(path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    size_t remaining = req->content_len, got = 0;
    bool ioerr = false;
    while (remaining > 0) {
        size_t want = remaining < SEND_CHUNK ? remaining : SEND_CHUNK;
        int r = httpd_req_recv(req, buf, want);
        if (r <= 0) { ioerr = true; break; }
        if (fwrite(buf, 1, r, f) != (size_t)r) { ioerr = true; break; }
        remaining -= r;
        got += r;
    }
    free(buf);
    fclose(f);

    if (ioerr) {
        unlink(path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv/write failed");
        return ESP_FAIL;
    }

    char msg[96];
    int len = snprintf(msg, sizeof(msg), "已保存 %s (%zu bytes)", name, got);
    ESP_LOGI(TAG, "upload: %s %u bytes", name, (unsigned)got);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, msg, len);
}

static esp_err_t delete_get_handler(httpd_req_t *req)
{
    char name[64];
    if (!get_query(req, "name", name, sizeof(name)) || !valid_name(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_FAIL;
    }
    char path[128];
    custom_path(name, path, sizeof(path));
    bool ok = (unlink(path) == 0);
    ESP_LOGI(TAG, "delete %s: %d", name, ok);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, ok ? "已删除" : "删除失败", HTTPD_RESP_USE_STRLEN);
}

/* 把存储中的 payload 通过 TCP 推给 PS5 上 elfldr 监听的 9021 端口 */
static esp_err_t push_get_handler(httpd_req_t *req)
{
    char name[64], ip[16];
    if (!get_query(req, "name", name, sizeof(name)) || !valid_name(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_FAIL;
    }
    if (!get_query(req, "ip", ip, sizeof(ip)) || !valid_ip(ip)) {
        if (g_ps5_ip[0] == '\0' || !valid_ip(g_ps5_ip)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no ps5 ip");
            return ESP_FAIL;
        }
        strncpy(ip, g_ps5_ip, sizeof(ip));
    }

    char path[128];
    custom_path(name, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such file");
        return ESP_FAIL;
    }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "socket failed");
        return ESP_FAIL;
    }
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(ELFLDR_PORT);
    inet_pton(AF_INET, ip, &dst.sin_addr);

    char msg[128];
    if (connect(s, (struct sockaddr *)&dst, sizeof(dst)) != 0) {
        snprintf(msg, sizeof(msg), "连接 %s:9021 失败：elfldr 未运行？PS5 是否在线？", ip);
        closesocket(s);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    }

    FILE *f = fopen(path, "rb");
    char *buf = (char *)malloc(SEND_CHUNK);
    if (f == NULL || buf == NULL) {
        if (f) fclose(f);
        free(buf);
        closesocket(s);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
        return ESP_FAIL;
    }

    size_t sent_total = 0;
    bool ioerr = false;
    size_t n;
    while ((n = fread(buf, 1, SEND_CHUNK, f)) > 0) {
        size_t off = 0;
        while (off < n) {
            ssize_t w = send(s, buf + off, n - off, MSG_NOSIGNAL);
            if (w <= 0) { ioerr = true; break; }
            off += (size_t)w;
        }
        if (ioerr) break;
        sent_total += n;
    }
    free(buf);
    fclose(f);
    closesocket(s);

    if (ioerr) {
        snprintf(msg, sizeof(msg), "传输中断：%zu/%ld bytes", sent_total, (long)st.st_size);
        ESP_LOGE(TAG, "push interrupted");
    } else {
        snprintf(msg, sizeof(msg), "已推送 %s (%zu bytes) 到 %s:9021", name, sent_total, ip);
        ESP_LOGI(TAG, "push ok: %s -> %s", name, ip);
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

/* ---------- webserver ---------- */

static httpd_handle_t start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = HTTP_PORT;
    cfg.max_open_sockets = 12;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) return NULL;

    /* 精确路由必须先于通配路由注册 */
    const httpd_uri_t uris[] = {
        { .uri = (char *)"/manage",    .method = HTTP_GET,  .handler = manage_get_handler,  .user_ctx = NULL },
        { .uri = (char *)"/api/list",  .method = HTTP_GET,  .handler = api_list_get_handler, .user_ctx = NULL },
        { .uri = (char *)"/upload",    .method = HTTP_POST, .handler = upload_post_handler,  .user_ctx = NULL },
        { .uri = (char *)"/delete",    .method = HTTP_GET,  .handler = delete_get_handler,   .user_ctx = NULL },
        { .uri = (char *)"/push",      .method = HTTP_GET,  .handler = push_get_handler,     .user_ctx = NULL },
        { .uri = (char *)"/*",         .method = HTTP_GET,  .handler = static_get_handler,   .user_ctx = NULL },
    };
    for (const httpd_uri_t &u : uris) {
        if (httpd_register_uri_handler(srv, &u) != ESP_OK) {
            httpd_stop(srv);
            return NULL;
        }
    }
    return srv;
}

/* ---------- main ---------- */

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    mount_littlefs();
    wifi_start_ap();

    if (start_webserver() == NULL) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }

    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "PS5 连接热点 \"%s\"（密码 %s）", CONFIG_APP_SSID, CONFIG_APP_PASS);
    ESP_LOGI(TAG, "PS5 浏览器打开 : http://192.168.4.1:%d/", HTTP_PORT);
    ESP_LOGI(TAG, "手机/电脑管理页: http://192.168.4.1:%d/manage", HTTP_PORT);
    ESP_LOGI(TAG, "================================================");
}
