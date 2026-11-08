#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reent.h>
#include "esp_check.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "settings.h"
#include "shell.h"
#include "telnet.h"

static const char *TAG = "telnet";

#define IAC 255
#define DONT 254
#define DO 253
#define WILL 251
#define SB 250
#define SE 240
#define OPT_ECHO 1
#define OPT_SGA 3
#define OPT_NAWS 31

#define PROMPT "esp32dif> "
#define LINE_LEN 255
#define HISTORY_LEN 16
#define TRIES 3
#define ESC_TIMEOUT_MS 100
#define LOGIN_TIMEOUT_S 60

enum {
    RD_TIMEOUT = -1,
    RD_EOF = -2,
    RD_IDLE = -3,
    KEY_IGNORED = 0,
    KEY_UP = 1000,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_DEL,
};

typedef struct {
    int sock;
    FILE *out;
    bool in_cr;
    bool out_cr;
    size_t cols;
    char ip[16];
    char *history[HISTORY_LEN];
    int history_len;
} session_t;

static volatile bool s_active;

static bool send_all(int sock, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = send(sock, p, len, 0);
        if (n <= 0) {
            return false;
        }
        p += n;
        len -= n;
    }
    return true;
}

static int out_write(void *cookie, const char *buf, int len)
{
    session_t *s = cookie;
    char tmp[128];
    size_t n = 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = buf[i];
        if (n > sizeof(tmp) - 2) {
            if (!send_all(s->sock, tmp, n)) {
                return -1;
            }
            n = 0;
        }
        if ((c == '\n' && !s->out_cr) || c == IAC) {
            tmp[n++] = c == IAC ? IAC : '\r';
        }
        tmp[n++] = c;
        s->out_cr = c == '\r';
    }
    return n && !send_all(s->sock, tmp, n) ? -1 : len;
}

static int recv_byte(session_t *s, int timeout_ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s->sock, &rfds);
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    int r = select(s->sock + 1, &rfds, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
    if (r == 0) {
        return RD_TIMEOUT;
    }
    unsigned char c;
    return r > 0 && recv(s->sock, &c, 1, 0) == 1 ? c : RD_EOF;
}

static void subnegotiation(session_t *s)
{
    unsigned char buf[8];
    size_t n = 0;
    for (;;) {
        int c = recv_byte(s, 1000);
        if (c == IAC) {
            c = recv_byte(s, 1000);
            if (c == SE) {
                break;
            }
        }
        if (c < 0) {
            return;
        }
        if (n < sizeof(buf)) {
            buf[n++] = c;
        }
    }
    if (n >= 5 && buf[0] == OPT_NAWS) {
        size_t cols = buf[1] << 8 | buf[2];
        if (cols >= 20) {
            s->cols = cols;
        }
    }
}

static int get_byte(session_t *s, int timeout_ms)
{
    for (;;) {
        int c = recv_byte(s, timeout_ms);
        if (c == IAC) {
            c = recv_byte(s, 1000);
            if (c == SB) {
                subnegotiation(s);
            } else if (c >= WILL && c <= DONT) {
                c = recv_byte(s, 1000);
            }
            if (c == RD_EOF) {
                return c;
            }
            continue;
        }
        if (c < 0) {
            return c;
        }
        bool after_cr = s->in_cr;
        s->in_cr = c == '\r';
        if (after_cr && (c == '\n' || c == 0)) {
            continue;
        }
        return c == '\n' ? '\r' : c;
    }
}

static int read_key(session_t *s, int64_t deadline_us)
{
    int c;
    do {
        int timeout_ms = -1;
        if (deadline_us) {
            int64_t left = deadline_us - esp_timer_get_time();
            if (left <= 0) {
                return RD_IDLE;
            }
            timeout_ms = left / 1000 + 1;
        }
        c = get_byte(s, timeout_ms);
    } while (c == RD_TIMEOUT);
    if (c != 0x1b) {
        return c;
    }

    c = get_byte(s, ESC_TIMEOUT_MS);
    if (c != '[' && c != 'O') {
        return KEY_IGNORED;
    }
    c = get_byte(s, ESC_TIMEOUT_MS);
    switch (c) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    }
    if (c < '0' || c > '9' || get_byte(s, ESC_TIMEOUT_MS) != '~') {
        return KEY_IGNORED;
    }
    switch (c) {
    case '1': case '7': return KEY_HOME;
    case '4': case '8': return KEY_END;
    case '3': return KEY_DEL;
    }
    return KEY_IGNORED;
}

static int64_t idle_deadline(int timeout_s)
{
    return timeout_s ? esp_timer_get_time() + timeout_s * 1000000LL : 0;
}

static void refresh(session_t *s, const char *buf, size_t len, size_t pos)
{
    size_t plen = strlen(PROMPT), start = 0;
    while (plen + pos - start >= s->cols) {
        start++;
    }
    size_t shown = len - start;
    if (plen + shown > s->cols) {
        shown = s->cols - plen;
    }
    fprintf(s->out, "\r" PROMPT "%.*s\x1b[K\r", (int)shown, buf + start);
    if (plen + pos - start) {
        fprintf(s->out, "\x1b[%uC", (unsigned)(plen + pos - start));
    }
    fflush(s->out);
}

static void complete(session_t *s, char *buf, size_t *len, size_t *pos)
{
    if (*pos != *len || strchr(buf, ' ')) {
        return;
    }
    linenoiseCompletions lc = { 0 };
    esp_console_get_completion(buf, &lc);
    if (lc.len == 1) {
        snprintf(buf, LINE_LEN + 1, "%s ", lc.cvec[0]);
    } else if (lc.len > 1) {
        size_t common = strlen(lc.cvec[0]);
        for (size_t i = 1; i < lc.len; i++) {
            while (strncmp(lc.cvec[0], lc.cvec[i], common)) {
                common--;
            }
        }
        if (common > *len) {
            snprintf(buf, LINE_LEN + 1, "%.*s", (int)common, lc.cvec[0]);
        } else {
            fputs("\n", s->out);
            for (size_t i = 0; i < lc.len; i++) {
                fprintf(s->out, "%s  ", lc.cvec[i]);
            }
            fputs("\n", s->out);
        }
    }
    for (size_t i = 0; i < lc.len; i++) {
        free(lc.cvec[i]);
    }
    free(lc.cvec);
    *len = *pos = strlen(buf);
}

static void history_add(session_t *s, const char *line)
{
    if (!line[0] || (s->history_len && strcmp(s->history[s->history_len - 1], line) == 0)) {
        return;
    }
    char *copy = strdup(line);
    if (!copy) {
        return;
    }
    if (s->history_len == HISTORY_LEN) {
        free(s->history[0]);
        memmove(s->history, s->history + 1, (HISTORY_LEN - 1) * sizeof(s->history[0]));
        s->history_len--;
    }
    s->history[s->history_len++] = copy;
}

static int read_line(session_t *s, char *buf)
{
    char draft[LINE_LEN + 1] = "";
    size_t len = 0, pos = 0;
    int hist = s->history_len;
    buf[0] = 0;
    refresh(s, buf, len, pos);
    for (;;) {
        int c = read_key(s, idle_deadline(CONFIG_APP_TELNET_IDLE_S));
        if (c < 0) {
            return c;
        }
        switch (c) {
        case '\r':
            fputs("\r\n", s->out);
            return len;
        case 3:
            fputs("^C\r\n", s->out);
            buf[0] = 0;
            return 0;
        case 4:
            if (!len) {
                fputs("\r\n", s->out);
                return RD_EOF;
            }
            __attribute__((fallthrough));
        case KEY_DEL:
            if (pos < len) {
                memmove(buf + pos, buf + pos + 1, len - pos);
                len--;
            }
            break;
        case 127:
        case 8:
            if (pos) {
                memmove(buf + pos - 1, buf + pos, len - pos + 1);
                pos--;
                len--;
            }
            break;
        case 1:
        case KEY_HOME:
            pos = 0;
            break;
        case 5:
        case KEY_END:
            pos = len;
            break;
        case 2:
        case KEY_LEFT:
            pos -= pos > 0;
            break;
        case 6:
        case KEY_RIGHT:
            pos += pos < len;
            break;
        case 21:
            memmove(buf, buf + pos, len - pos + 1);
            len -= pos;
            pos = 0;
            break;
        case 11:
            len = pos;
            break;
        case 23: {
            size_t from = pos;
            while (from && buf[from - 1] == ' ') {
                from--;
            }
            while (from && buf[from - 1] != ' ') {
                from--;
            }
            memmove(buf + from, buf + pos, len - pos + 1);
            len -= pos - from;
            pos = from;
            break;
        }
        case 12:
            fputs("\x1b[H\x1b[2J", s->out);
            break;
        case 16:
        case KEY_UP:
        case 14:
        case KEY_DOWN: {
            int next = hist + (c == 16 || c == KEY_UP ? -1 : 1);
            if (next < 0 || next > s->history_len) {
                break;
            }
            if (hist == s->history_len) {
                strlcpy(draft, buf, sizeof(draft));
            }
            hist = next;
            strlcpy(buf, hist == s->history_len ? draft : s->history[hist], LINE_LEN + 1);
            len = pos = strlen(buf);
            break;
        }
        case '\t':
            complete(s, buf, &len, &pos);
            break;
        default:
            if (c >= 0x20 && c < 0x7f && len < LINE_LEN) {
                memmove(buf + pos + 1, buf + pos, len - pos + 1);
                buf[pos++] = c;
                len++;
            }
            break;
        }
        buf[len] = 0;
        refresh(s, buf, len, pos);
    }
}

static bool read_password(session_t *s, char *buf, size_t size)
{
    size_t len = 0;
    int64_t deadline = idle_deadline(LOGIN_TIMEOUT_S);
    for (;;) {
        int c = read_key(s, deadline);
        if (c < 0 || c == 3 || c == 4) {
            return false;
        }
        if (c == '\r') {
            buf[len] = 0;
            fputs("\r\n", s->out);
            return true;
        }
        if ((c == 127 || c == 8) && len) {
            len--;
        } else if (c >= 0x20 && c < 0x7f && len < size - 1) {
            buf[len++] = c;
        }
    }
}

static bool login(session_t *s)
{
    for (int i = 0; i < TRIES; i++) {
        fputs("password: ", s->out);
        fflush(s->out);
        char pw[64];
        if (!read_password(s, pw, sizeof(pw))) {
            return false;
        }
        if (settings_check_password(pw)) {
            return true;
        }
        ESP_LOGW(TAG, "%s: wrong password", s->ip);
        vTaskDelay(pdMS_TO_TICKS(2000));
        fputs("wrong password\n", s->out);
    }
    return false;
}

static void shell_loop(session_t *s)
{
    fputs("Type 'help' to get the list of commands, 'exit' or Ctrl+D to quit.\n", s->out);
    char line[LINE_LEN + 1];
    int n;
    while ((n = read_line(s, line)) >= 0) {
        if (strcmp(line, "exit") == 0 || strcmp(line, "logout") == 0) {
            return;
        }
        if (n) {
            history_add(s, line);
            shell_exec(line);
        }
    }
    if (n == RD_IDLE) {
        fputs("\r\nidle timeout\n", s->out);
    }
}

static void session_task(void *arg)
{
    session_t *s = arg;
    FILE *in = NULL;
    s->cols = 80;
    s->out = funopen(s, NULL, out_write, NULL, NULL);
    if (s->out) {
        setvbuf(s->out, NULL, _IOLBF, 256);
        const unsigned char options[] = { IAC, WILL, OPT_ECHO, IAC, WILL, OPT_SGA, IAC, DO, OPT_SGA, IAC, DO, OPT_NAWS };
        send_all(s->sock, options, sizeof(options));
        fputs("esp32dif shell\n", s->out);
        if (login(s) && (in = fdopen(s->sock, "r"))) {
            ESP_LOGI(TAG, "%s: logged in", s->ip);
            stdin = in;
            stdout = stderr = s->out;
            shell_loop(s);
            stdin = _GLOBAL_REENT->_stdin;
            stdout = _GLOBAL_REENT->_stdout;
            stderr = _GLOBAL_REENT->_stderr;
        }
        fclose(s->out);
    }
    if (in) {
        fclose(in);
    } else {
        close(s->sock);
    }
    ESP_LOGI(TAG, "%s: disconnected", s->ip);
    for (int i = 0; i < s->history_len; i++) {
        free(s->history[i]);
    }
    free(s);
    s_active = false;
    vTaskDelete(NULL);
}

static void set_options(int sock)
{
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int on = 1, idle = 60, intvl = 10, cnt = 3;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

static void server_task(void *arg)
{
    int listener = (int)(intptr_t)arg;
    for (;;) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        int sock = accept(listener, (struct sockaddr *)&addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "accept: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        char ip[16];
        inet_ntoa_r(addr.sin_addr, ip, sizeof(ip));
        if (s_active) {
            ESP_LOGW(TAG, "%s: rejected, another session is active", ip);
            static const char busy[] = "busy: another telnet session is active\r\n";
            send(sock, busy, sizeof(busy) - 1, 0);
            close(sock);
            continue;
        }
        set_options(sock);
        session_t *s = calloc(1, sizeof(*s));
        if (!s) {
            close(sock);
            continue;
        }
        s->sock = sock;
        strlcpy(s->ip, ip, sizeof(s->ip));
        ESP_LOGI(TAG, "%s: connected", ip);
        s_active = true;
        if (xTaskCreate(session_task, "telnet", 8192, s, 2, NULL) != pdPASS) {
            ESP_LOGE(TAG, "%s: no memory for session", ip);
            close(sock);
            free(s);
            s_active = false;
        }
    }
}

esp_err_t telnet_start(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    ESP_RETURN_ON_FALSE(sock >= 0, ESP_FAIL, TAG, "socket: errno %d", errno);
    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_APP_TELNET_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(sock, 1) != 0) {
        ESP_LOGE(TAG, "listen on port %d: errno %d", CONFIG_APP_TELNET_PORT, errno);
        close(sock);
        return ESP_FAIL;
    }
    if (xTaskCreate(server_task, "telnetd", 3072, (void *)(intptr_t)sock, 2, NULL) != pdPASS) {
        close(sock);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "listening on port %d", CONFIG_APP_TELNET_PORT);
    return ESP_OK;
}
