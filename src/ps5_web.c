#include "ps5_web.h"
#include "ps5_log.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define WEB_REQUEST_SIZE 1024u
#define WEB_RESPONSE_SIZE 16384u
#define WEB_BODY_OFFSET 256u

#ifndef PS5_WEB_LOG_PATH
#define PS5_WEB_LOG_PATH "/data/ps5-xb360/receiver.log"
#endif
#ifndef PS5_WEB_TELEMETRY_PATH
#define PS5_WEB_TELEMETRY_PATH "/data/ps5-xb360/telemetry.csv"
#endif

struct ps5_web {
    int listen_fd;
    int client_fd;
    char request[WEB_REQUEST_SIZE];
    size_t request_length;
    char response[WEB_RESPONSE_SIZE];
    size_t response_length;
    size_t response_sent;
    int download_fd;
    off_t download_remaining;
    int pending_action;
};

static struct ps5_web g_web;

static void build_response(ps5_web_t *web, int code, const char *type,
                           size_t body_length);

static const char g_page[] =
"<!doctype html><html lang='ru'><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>PS5 Xbox 360 Receiver</title><style>"
":root{color-scheme:dark;font-family:system-ui;background:#101318;color:#e7edf5}"
"body{max-width:960px;margin:auto;padding:20px}h1{font-size:1.5rem}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:10px}"
".card{background:#1b2028;border:1px solid #303846;border-radius:10px;padding:14px}"
".on{color:#6ee7a8}.off{color:#9ca8b8}button{padding:10px 14px;margin:8px 8px 8px 0}"
".metrics{font-size:.82rem;line-height:1.45;color:#c2ccda;margin-top:8px}"
"pre{background:#090b0f;padding:12px;border-radius:8px;white-space:pre-wrap;min-height:220px}"
"small{color:#9ca8b8}</style><h1>Xbox 360 Receiver → PS5</h1>"
"<div id='summary' class='card'>Загрузка…</div><h2>Контроллеры</h2>"
"<div id='slots' class='grid'></div><p>"
"<button onclick=act('inquiry')>Повторить inquiry</button>"
"<a href='/api/log/download' download><button>Скачать полный лог</button></a>"
"<button onclick=stopPayload()>Остановить payload</button></p>"
"<h2>Телеметрия</h2><div class='card'><div id='captureStatus'>Готово к записи</div>"
"<button id='captureStart' onclick=startTelemetry()>Записать 10 секунд</button>"
"<a href='/api/telemetry/download' download><button id='csvBtn' disabled>Скачать CSV</button></a></div>"
"<h2>Динамический лог</h2><pre id='log'></pre>"
"<small>Обновление раз в секунду · фактический порт указан в адресной строке</small><script>"
"async function refresh(){try{let s=await(await fetch('/api/status',{cache:'no-store'})).json();"
"summary.textContent=`PID ${s.pid} · uptime ${s.uptime}s · ${s.node} · ${s.vid}:${s.pidUsb} · USB errors ${s.usbErrors}`;"
"slots.innerHTML=s.slots.map((x,i)=>{let t=x.telemetry;return `<div class=card><b>Slot ${i+1}</b><br><span class=${x.connected?'on':'off'}>${x.connected?'подключён':'свободен'}</span><br>${x.psAvailable?'PS готов':'PS недоступен / подготовка'}<br><button ${x.psAvailable?'':'disabled'} onclick=pressPs(${i+1})>Нажать PS</button><div class=metrics>USB: ${t.usb} · input: ${t.input} · PS5: ${t.submitted}<br>объединено: ${t.coalesced} · ошибок: ${t.failed} · malformed: ${t.malformed}<br>предполагаемые пропуски: ≈${t.gaps}<br>interval µs p50/p95/p99/max: ${t.intervalP50}/${t.intervalP95}/${t.intervalP99}/${t.intervalMax}<br>bridge µs p50/p95/p99/max: ${t.bridgeP50}/${t.bridgeP95}/${t.bridgeP99}/${t.bridgeMax}<br>InsertData µs p50/p95/p99/max: ${t.insertP50}/${t.insertP95}/${t.insertP99}/${t.insertMax}</div></div>`}).join('');"
"let c=s.capture;captureStatus.textContent=c.active?`Запись: ${(c.remainingMs/1000).toFixed(1)} с · ${c.records} строк`:(c.csvReady?`CSV готов: ${c.records} строк · пропущено ${c.dropped}`:'Готово к записи');captureStart.disabled=c.active;csvBtn.disabled=!c.csvReady;"
"log.textContent=await(await fetch('/api/log',{cache:'no-store'})).text();log.scrollTop=log.scrollHeight;"
"}catch(e){summary.textContent='Нет связи с payload: '+e}}"
"async function act(a){await fetch('/api/'+a,{method:'POST'});setTimeout(refresh,200)}"
"async function pressPs(i){await fetch('/api/ps/'+i,{method:'POST'});setTimeout(refresh,200)}"
"async function startTelemetry(){await fetch('/api/telemetry/start',{method:'POST'});setTimeout(refresh,200)}"
"function stopPayload(){if(confirm('Остановить payload и освободить USB?'))act('stop')}"
"refresh();setInterval(refresh,1000)</script></html>";

static void close_client(ps5_web_t *web) {
    if (web->client_fd >= 0) close(web->client_fd);
    if (web->download_fd >= 0) close(web->download_fd);
    web->client_fd = -1;
    web->download_fd = -1;
    web->download_remaining = 0;
    web->request_length = 0u;
    web->response_length = 0u;
    web->response_sent = 0u;
}

static void build_download_response(ps5_web_t *web, const char *path,
                                    const char *filename) {
    struct stat info;
    int header_length;
    web->download_fd = open(path, O_RDONLY);
    if (web->download_fd < 0 || fstat(web->download_fd, &info) != 0 ||
        info.st_size < 0) {
        char *body = web->response + WEB_BODY_OFFSET;
        if (web->download_fd >= 0) close(web->download_fd);
        web->download_fd = -1;
        memcpy(body, "file unavailable\n", 17u);
        build_response(web, 404, "text/plain; charset=utf-8", 17u);
        return;
    }
    web->download_remaining = info.st_size;
    header_length = snprintf(web->response, sizeof(web->response),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Content-Length: %lld\r\nCache-Control: no-store\r\n"
        "Connection: close\r\n\r\n", filename, (long long)info.st_size);
    if (header_length < 0 || (size_t)header_length >= sizeof(web->response)) {
        close_client(web);
        return;
    }
    web->response_length = (size_t)header_length;
    web->response_sent = 0u;
}

static int make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static size_t json_string(char *output, size_t capacity, const char *input) {
    size_t used = 0u;
    if (capacity == 0u) return 0u;
    output[used++] = '"';
    for (; input != NULL && *input != '\0' && used + 2u < capacity; ++input) {
        unsigned char c = (unsigned char)*input;
        if (c == '"' || c == '\\') output[used++] = '\\';
        if (c >= 0x20u) output[used++] = (char)c;
    }
    if (used < capacity) output[used++] = '"';
    if (used < capacity) output[used] = '\0';
    return used;
}

static size_t build_status(char *output, size_t capacity,
                           const ps5_web_status_t *status) {
    char node[160];
    char error[256];
    size_t used;
    json_string(node, sizeof(node), status->receiver_node);
    json_string(error, sizeof(error), status->last_usb_error);
    used = (size_t)snprintf(output, capacity,
        "{\"pid\":%d,\"uptime\":%lu,\"node\":%s,"
        "\"vid\":\"%04X\",\"pidUsb\":\"%04X\",\"usbErrors\":%lu,"
        "\"lastError\":%s,\"capture\":{\"active\":%s,"
        "\"csvReady\":%s,\"remainingMs\":%u,\"records\":%lu,"
        "\"dropped\":%lu},\"slots\":[",
        status->pid, status->uptime_seconds, node, status->receiver_vendor,
        status->receiver_product, status->usb_errors, error,
        status->capture.active ? "true" : "false",
        status->capture.csv_ready ? "true" : "false",
        status->capture.remaining_ms, status->capture.records,
        status->capture.dropped);
    for (unsigned i = 0u; i < status->slot_count && used < capacity; ++i) {
        const ps5_telemetry_slot_snapshot_t *t = &status->slots[i].telemetry;
        used += (size_t)snprintf(output + used, capacity - used,
            "%s{\"connected\":%s,\"psAvailable\":%s,\"reports\":%lu,"
            "\"telemetry\":{\"usb\":%lu,\"input\":%lu,\"malformed\":%lu,"
            "\"submitted\":%lu,\"coalesced\":%lu,\"failed\":%lu,"
            "\"gaps\":%lu,\"intervalP50\":%lu,\"intervalP95\":%lu,"
            "\"intervalP99\":%lu,\"intervalMax\":%lu,"
            "\"bridgeP50\":%lu,\"bridgeP95\":%lu,\"bridgeP99\":%lu,"
            "\"bridgeMax\":%lu,\"insertP50\":%lu,\"insertP95\":%lu,"
            "\"insertP99\":%lu,\"insertMax\":%lu}}",
            i ? "," : "",
            status->slots[i].connected ? "true" : "false",
            status->slots[i].ps_available ? "true" : "false",
            status->slots[i].reports, t->usb_packets, t->input_reports,
            t->malformed_packets, t->submitted_reports, t->coalesced_reports,
            t->submit_failures, t->estimated_gaps, t->interval_p50_us,
            t->interval_p95_us, t->interval_p99_us, t->interval_max_us,
            t->bridge_p50_us, t->bridge_p95_us, t->bridge_p99_us,
            t->bridge_max_us, t->insert_p50_us, t->insert_p95_us,
            t->insert_p99_us, t->insert_max_us);
    }
    if (used + 3u < capacity) {
        memcpy(output + used, "]}", 3u);
        used += 2u;
    }
    return used;
}

static void build_response(ps5_web_t *web, int code, const char *type,
                           size_t body_length) {
    char header[WEB_BODY_OFFSET];
    int header_length = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
        code, code == 200 ? "OK" : "Not Found", type, body_length);
    if (header_length < 0 || (size_t)header_length >= sizeof(header) ||
        (size_t)header_length + body_length > sizeof(web->response)) {
        close_client(web);
        return;
    }
    memmove(web->response + header_length, web->response + WEB_BODY_OFFSET,
            body_length);
    memcpy(web->response, header, (size_t)header_length);
    web->response_length = (size_t)header_length + body_length;
    web->response_sent = 0u;
}

static void route_request(ps5_web_t *web, const ps5_web_status_t *status) {
    size_t body_length = 0u;
    char *body = web->response + WEB_BODY_OFFSET;
    size_t body_capacity = sizeof(web->response) - WEB_BODY_OFFSET;
    if (strncmp(web->request, "GET /api/status ", 16u) == 0) {
        body_length = build_status(body, body_capacity, status);
        build_response(web, 200, "application/json; charset=utf-8", body_length);
    } else if (strncmp(web->request, "GET /api/log/download ", 22u) == 0) {
        build_download_response(web, PS5_WEB_LOG_PATH,
                                "ps5-xb360-receiver.log");
    } else if (strncmp(web->request, "GET /api/telemetry/download ", 28u) == 0) {
        build_download_response(web, PS5_WEB_TELEMETRY_PATH,
                                "ps5-xb360-telemetry.csv");
    } else if (strncmp(web->request, "GET /api/log ", 13u) == 0) {
        body_length = ps5_log_tail(body, body_capacity);
        build_response(web, 200, "text/plain; charset=utf-8", body_length);
    } else if (strncmp(web->request, "POST /api/inquiry ", 18u) == 0) {
        memcpy(body, "ok\n", 3u); web->pending_action = PS5_WEB_ACTION_INQUIRY;
        build_response(web, 200, "text/plain", 3u);
    } else if (strncmp(web->request, "POST /api/stop ", 15u) == 0) {
        memcpy(body, "stopping\n", 9u); web->pending_action = PS5_WEB_ACTION_STOP;
        build_response(web, 200, "text/plain", 9u);
    } else if (strncmp(web->request, "POST /api/telemetry/start ", 26u) == 0) {
        memcpy(body, "ok\n", 3u);
        web->pending_action = PS5_WEB_ACTION_TELEMETRY_START;
        build_response(web, 200, "text/plain", 3u);
    } else if (strncmp(web->request, "POST /api/ps/", 13u) == 0 &&
               web->request[13] >= '1' && web->request[13] <= '4' &&
               web->request[14] == ' ') {
        unsigned slot = (unsigned)(web->request[13] - '1');
        memcpy(body, "ok\n", 3u);
        web->pending_action = PS5_WEB_ACTION_PS_SLOT(slot);
        build_response(web, 200, "text/plain", 3u);
    } else if (strncmp(web->request, "GET / ", 6u) == 0) {
        body_length = sizeof(g_page) - 1u;
        memcpy(body, g_page, body_length);
        build_response(web, 200, "text/html; charset=utf-8", body_length);
    } else {
        memcpy(body, "not found\n", 10u);
        build_response(web, 404, "text/plain", 10u);
    }
}

ps5_web_t *ps5_web_open(uint16_t port, char *why, int why_length) {
    ps5_web_t *web = &g_web;
    struct sockaddr_in address;
    int reuse = 1;
    memset(web, 0, sizeof(*web));
    web->listen_fd = web->client_fd = web->download_fd = -1;
    web->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (web->listen_fd < 0) goto fail;
    setsockopt(web->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (!make_nonblocking(web->listen_fd)) goto fail;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(web->listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(web->listen_fd, 4) != 0) goto fail;
    snprintf(why, (size_t)why_length, "HTTP port %u", (unsigned)port);
    return web;
fail:
    snprintf(why, (size_t)why_length, "HTTP port %u: %s", (unsigned)port,
             strerror(errno));
    ps5_web_close(web);
    return NULL;
}

int ps5_web_poll(ps5_web_t *web, const ps5_web_status_t *status) {
    int action = PS5_WEB_ACTION_NONE;
    if (web == NULL) return action;
    if (web->client_fd < 0) {
        web->client_fd = accept(web->listen_fd, NULL, NULL);
        if (web->client_fd >= 0) make_nonblocking(web->client_fd);
    }
    if (web->client_fd < 0) return action;
    if (web->response_length > web->response_sent) {
        ssize_t sent = send(web->client_fd, web->response + web->response_sent,
                            web->response_length - web->response_sent, 0);
        if (sent > 0) web->response_sent += (size_t)sent;
        else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            close_client(web);
        if (web->response_sent == web->response_length) {
            if (web->download_fd >= 0 && web->download_remaining > 0) {
                size_t wanted = sizeof(web->response);
                ssize_t count;
                if ((off_t)wanted > web->download_remaining)
                    wanted = (size_t)web->download_remaining;
                count = read(web->download_fd, web->response, wanted);
                if (count > 0) {
                    web->download_remaining -= count;
                    web->response_length = (size_t)count;
                    web->response_sent = 0u;
                    return action;
                }
                close_client(web);
                return action;
            }
            action = web->pending_action;
            web->pending_action = PS5_WEB_ACTION_NONE;
            close_client(web);
        }
        return action;
    }
    {
        ssize_t received = recv(web->client_fd,
            web->request + web->request_length,
            sizeof(web->request) - web->request_length - 1u, 0);
        if (received > 0) {
            web->request_length += (size_t)received;
            web->request[web->request_length] = '\0';
            if (strstr(web->request, "\r\n\r\n") != NULL)
                route_request(web, status);
        } else if (received == 0 ||
                   (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            close_client(web);
        }
        if (web->request_length + 1u >= sizeof(web->request)) close_client(web);
    }
    return action;
}

void ps5_web_close(ps5_web_t *web) {
    if (web == NULL) return;
    close_client(web);
    if (web->listen_fd >= 0) close(web->listen_fd);
    web->listen_fd = -1;
}
