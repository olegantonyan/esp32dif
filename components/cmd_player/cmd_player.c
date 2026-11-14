#include <stdio.h>
#include "esp_check.h"
#include "esp_console.h"
#include "lwip/inet.h"
#include "remote.h"
#include "shell.h"
#include "cmd_player.h"

static const char *TAG = "cmd_player";

static int send_action(remote_action_t action)
{
    int sent = remote_send(action);
    if (!sent) {
        fprintf(stderr, "no PC connected\n");
        return 1;
    }
    remote_status_t r;
    remote_get(&r);
    char ip[16];
    printf("%s -> %s%s\n", remote_action_name(action), inet_ntoa_r(r.last_ip, ip, sizeof(ip)), sent > 1 ? " and others" : "");
    return 0;
}

static int cmd_prev(int argc, char **argv) { return send_action(REMOTE_PREVIOUS); }
static int cmd_play(int argc, char **argv) { return send_action(REMOTE_PLAY_PAUSE); }
static int cmd_next(int argc, char **argv) { return send_action(REMOTE_NEXT); }
static int cmd_vol_up(int argc, char **argv) { return send_action(REMOTE_VOLUME_UP); }
static int cmd_vol_down(int argc, char **argv) { return send_action(REMOTE_VOLUME_DOWN); }
static int cmd_mute(int argc, char **argv) { return send_action(REMOTE_MUTE); }

static int cmd_status(int argc, char **argv)
{
    remote_status_t r;
    remote_get(&r);
    if (!r.clients) {
        printf("no PC connected\n");
        return 0;
    }
    static remote_track_t t;
    for (int i = 0; i < r.clients; i++) {
        char ip[16];
        printf("%s", inet_ntoa_r(r.client_ip[i], ip, sizeof(ip)));
        if (!remote_track(r.client_ip[i], &t)) {
            printf(": no player\n");
            continue;
        }
        printf(": %s %s (%s): %s - %s", t.status, t.player, t.id, t.artist, t.title);
        if (t.volume >= 0) {
            printf(", vol %d%%%s", t.volume, t.muted ? " muted" : "");
        }
        printf("\n");
    }
    return 0;
}

static const shell_subcmd_t s_subcmds[] = {
    { .name = "prev", .help = "Previous track", .func = cmd_prev },
    { .name = "play", .help = "Play/pause", .func = cmd_play },
    { .name = "next", .help = "Next track", .func = cmd_next },
    { .name = "vol+", .help = "Volume up (PC sink volume)", .func = cmd_vol_up },
    { .name = "vol-", .help = "Volume down (PC sink volume)", .func = cmd_vol_down },
    { .name = "mute", .help = "Toggle mute", .func = cmd_mute },
    { .name = "status", .help = "Connected PCs, their player, track and volume", .func = cmd_status },
};

static int cmd_player(int argc, char **argv)
{
    return shell_subcmd_run("player", s_subcmds, sizeof(s_subcmds) / sizeof(s_subcmds[0]), argc, argv);
}

esp_err_t cmd_player_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "player",
        .help = "Control the PC media player: prev | play | next | vol+ | vol- | mute | status",
        .hint = "<command>",
        .func = cmd_player,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register player");
    return ESP_OK;
}
