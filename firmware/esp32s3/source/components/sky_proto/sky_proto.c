#include "sky_proto.h"

#include "board_pins.h"
#include "drill.h"
#include "enc_fn.h"
#include "key_link.h"
#include "mixer.h"
#include "net_link.h"
#include "score_play.h"
#include "sd_music.h"
#include "sky_session.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static SemaphoreHandle_t s_mu;
static uint16_t s_seq = 1;
static sky_map_get_fn s_map_get;
static sky_map_set_fn s_map_set;
static sky_result_v1_t s_last_result;

static uint8_t s_score_page[240];
static size_t s_score_page_len = 0;
static FILE *s_ble_up_fp;
static char s_ble_up_final[160];
static uint32_t s_ble_up_expect;
static uint32_t s_ble_up_written;

static int score_name_valid(const char *name)
{
    if (!name || !name[0] || name[0] == '.' || strchr(name, '/') || strchr(name, '\\') ||
        strstr(name, "..")) {
        return 0;
    }
    size_t l = strlen(name);
    if (l < 5 || l > 63) {
        return 0;
    }
    const char *ext = strrchr(name, '.');
    if (!ext) {
        return 0;
    }
    return strcasecmp(ext, ".txt") == 0 || strcasecmp(ext, ".json") == 0 ||
           strcasecmp(ext, ".sky") == 0;
}

static int cmp_cstr(const void *a, const void *b)
{
    return strcasecmp(*(const char *const *)a, *(const char *const *)b);
}

static void scan_dir_scores(const char *dir, char **names, int *total)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && *total < 256) {
        if (!score_name_valid(ent->d_name)) {
            continue;
        }
        names[*total] = strdup(ent->d_name);
        if (names[*total]) {
            (*total)++;
        }
    }
    closedir(d);
}

static void build_score_list_page(uint16_t offset)
{
    memset(s_score_page, 0, sizeof(s_score_page));
    s_score_page_len = 2;

    char **names = calloc(256, sizeof(char *));
    if (!names) {
        return;
    }
    int total = 0;
    scan_dir_scores(SD_MOUNT_POINT "/puzi", names, &total);
    if (total == 0) {
        scan_dir_scores(SD_MOUNT_POINT, names, &total);
    }
    if (total > 1) {
        qsort(names, (size_t)total, sizeof(names[0]), cmp_cstr);
    }

    s_score_page[0] = (uint8_t)total;
    uint8_t count = 0;
    size_t pos = 2;
    for (int i = (int)offset; i < total; ++i) {
        size_t nl = strlen(names[i]);
        if (pos + nl + 1 > sizeof(s_score_page) - 4) {
            break;
        }
        memcpy(&s_score_page[pos], names[i], nl);
        pos += nl;
        s_score_page[pos++] = '\n';
        count++;
    }
    s_score_page[1] = count;
    s_score_page_len = pos;

    for (int i = 0; i < total; ++i) {
        free(names[i]);
    }
    free(names);
}

static void lock(void)
{
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_mu) {
        xSemaphoreGive(s_mu);
    }
}

static uint16_t bump_unlocked(void)
{
    s_seq++;
    if (s_seq == 0) {
        s_seq = 1;
    }
    return s_seq;
}

size_t sky_proto_fill_score_list(uint8_t *out, size_t max_len)
{
    if (!out || max_len < 2) {
        return 0;
    }
    lock();
    if (s_score_page_len < 2) {
        build_score_list_page(0);
    }
    size_t n = s_score_page_len < max_len ? s_score_page_len : max_len;
    memcpy(out, s_score_page, n);
    unlock();
    return n;
}

void sky_proto_refresh_score_list(void)
{
    lock();
    build_score_list_page(0);
    bump_unlocked();
    unlock();
}

void sky_proto_set_map_ops(sky_map_get_fn get_fn, sky_map_set_fn set_fn)
{
    s_map_get = get_fn;
    s_map_set = set_fn;
    if (!s_mu) {
        s_mu = xSemaphoreCreateMutex();
    }
}

void sky_proto_bump(void)
{
    lock();
    bump_unlocked();
    unlock();
}

uint16_t sky_proto_state_seq(void)
{
    return s_seq;
}

static uint8_t transport_now(void)
{
    int mplay = 0, splay = 0, spause = 0;
    uint32_t ms = 0, dur = 0;
    unsigned pct = 0;
    sd_music_get_progress(&ms, &dur, &pct, &mplay);
    score_get_progress(&ms, &dur, &pct, &splay, &spause);
    if (mplay || splay) {
        return 1;
    }
    if (spause || sd_music_paused()) {
        return 2;
    }
    return 0;
}

static void fill_now_playing(sky_state_v1_t *out)
{
    uint32_t ms = 0, dur = 0;
    unsigned pct = 0;
    int mplay = 0, splay = 0, spause = 0;
    const char *path = NULL;
    sd_music_get_progress(&ms, &dur, &pct, &mplay);
    if (mplay || sd_music_paused()) {
        path = sd_music_path();
    } else {
        score_get_progress(&ms, &dur, &pct, &splay, &spause);
        if (splay || spause) {
            path = score_current_path();
        } else {
            ms = 0;
            dur = 0;
        }
    }
    out->play_ms = ms;
    out->dur_ms = dur;
    memset(out->title, 0, sizeof(out->title));
    if (path && path[0] && path[0] != '-') {
        const char *bn = strrchr(path, '/');
        bn = bn ? bn + 1 : path;
        strncpy(out->title, bn, sizeof(out->title) - 1);
    }
}

static uint8_t tempo_now(void)
{
    enc_fn_mode_t mode = enc_fn_mode();
    int t = 100;
    if (mode == ENC_FN_MUSIC) {
        t = sd_music_tempo_pct();
    } else {
        t = score_tempo_pct();
    }
    if (t < 50) {
        t = 50;
    }
    if (t > 200) {
        t = 200;
    }
    return (uint8_t)t;
}

void sky_proto_fill_state(sky_state_v1_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    drill_view_t dv;
    drill_view(&dv);

    out->ver = SKY_PROTO_VER;
    out->mode = (uint8_t)enc_fn_mode();
    out->map = (s_map_get && s_map_get()) ? 1u : 0u;
    out->volume = mixer_master_ui();
    out->transpose = (int8_t)mixer_transpose();
    out->tempo_pct = tempo_now();
    out->transport = transport_now();
    out->follow_state = (uint8_t)(dv.follow < 0 ? 0 : dv.follow);
    out->state_seq = s_seq;
    out->key_mask = (uint16_t)(key_link_keymask() & 0x7FFFu);
    out->expect_mask = dv.expect;
    out->follow_at = (uint16_t)(dv.at < 0 ? 0 : dv.at);
    out->follow_total = (uint16_t)(dv.total < 0 ? 0 : dv.total);
    out->bat_mv = (uint16_t)key_link_bat_mv();
    out->bat_pct = key_link_bat_soc();
    out->bat_level = (uint8_t)key_link_bat_level();
    out->chg_flags = key_link_chg_flags();
    out->flags = key_link_bat_ok() ? 0x01u : 0u;
    fill_now_playing(out);
}

static void fill_result(sky_result_v1_t *r, uint8_t op, uint16_t req, uint16_t code)
{
    r->ver = SKY_PROTO_VER;
    r->opcode = op;
    r->request_id = req;
    r->result_code = code;
    r->state_seq = s_seq;
    s_last_result = *r;
}

esp_err_t sky_proto_exec(const uint8_t *cmd, size_t len, sky_result_v1_t *result)
{
    if (!cmd || !result || len < SKY_CMD_HDR_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t ver = cmd[0];
    uint8_t op = cmd[1];
    uint16_t req = (uint16_t)cmd[2] | ((uint16_t)cmd[3] << 8);
    if (ver != SKY_PROTO_VER) {
        fill_result(result, op, req, SKY_RC_UNSUPPORTED);
        return ESP_OK;
    }

    lock();
    uint16_t code = SKY_RC_OK;
    switch (op) {
    case SKY_OP_GET_SNAPSHOT:
        /* no side effect; caller reads State */
        break;
    case SKY_OP_SET_MAP:
        if (len < 5 || !s_map_set) {
            code = SKY_RC_BAD_ARG;
            break;
        }
        if (cmd[4] > 1) {
            code = SKY_RC_BAD_ARG;
            break;
        }
        s_map_set(cmd[4] ? 1 : 0);
        bump_unlocked();
        break;
    case SKY_OP_SET_AUDIO: {
        if (len < 7) {
            code = SKY_RC_BAD_ARG;
            break;
        }
        uint8_t vol = cmd[4];
        int8_t tr = (int8_t)cmd[5];
        uint8_t tempo = cmd[6];
        if (vol > 100 || tr < -12 || tr > 12 || tempo < 50 || tempo > 200) {
            code = SKY_RC_BAD_ARG;
            break;
        }
        MixCmd mc = {.type = MIX_CMD_SET_MASTER, .velocity = vol};
        mixer_submit(&mc);
        mixer_set_transpose((int)tr);
        score_set_tempo_pct((int)tempo);
        sd_music_set_tempo_pct((int)tempo);
        bump_unlocked();
        break;
    }
    case SKY_OP_SET_MODE:
        if (len < 5) {
            code = SKY_RC_BAD_ARG;
            break;
        }
        if (cmd[4] >= (uint8_t)ENC_FN_MODE_COUNT) {
            code = SKY_RC_BAD_ARG;
            break;
        }
        enc_fn_set_mode((enc_fn_mode_t)cmd[4]);
        bump_unlocked();
        break;
    case SKY_OP_SET_TRANSPORT: {
        if (len < 5) {
            code = SKY_RC_BAD_ARG;
            break;
        }
        uint8_t act = cmd[4];
        enc_fn_mode_t mode = enc_fn_mode();
        if (act == SKY_TRANSPORT_STOP) {
            sd_music_cmd("stop");
            score_cmd_stop();
            drill_follow_stop();
            bump_unlocked();
            break;
        }
        if (act == SKY_TRANSPORT_PLAY) {
            enc_fn_activate();
            bump_unlocked();
            break;
        }
        if (act == SKY_TRANSPORT_PAUSE) {
            if (mode == ENC_FN_SCORE) {
                score_cmd_pause();
                bump_unlocked();
            } else if (mode == ENC_FN_MUSIC) {
                sd_music_pause_toggle();
                bump_unlocked();
            } else {
                code = SKY_RC_UNSUPPORTED; /* follow/record have no pause */
            }
            break;
        }
        if (act == SKY_TRANSPORT_NEXT || act == SKY_TRANSPORT_PREV) {
            int n = enc_fn_list_n();
            int idx = enc_fn_list_idx();
            if (n <= 0 || idx < 0) {
                code = SKY_RC_FAILED;
                break;
            }
            int next = (act == SKY_TRANSPORT_NEXT) ? (idx + 1) % n : (idx - 1 + n) % n;
            if (!enc_fn_select_idx(next)) {
                code = SKY_RC_FAILED;
                break;
            }
            /* Selecting alone is silent — start playback so App "下一首" works. */
            enc_fn_activate();
            bump_unlocked();
            break;
        }
        code = SKY_RC_BAD_ARG;
        break;
    }
    case SKY_OP_OPEN_ITEM: {
        if (len < 7) {
            code = SKY_RC_BAD_ARG;
            break;
        }
        uint8_t typ = cmd[4];
        uint16_t idx = (uint16_t)cmd[5] | ((uint16_t)cmd[6] << 8);
        enc_fn_mode_t mode = enc_fn_mode();
        uint8_t expect = SKY_ITEM_SCORE;
        if (mode == ENC_FN_PLAY) {
            expect = SKY_ITEM_BANK;
        } else if (mode == ENC_FN_MUSIC) {
            expect = SKY_ITEM_MUSIC;
        } else if (mode == ENC_FN_SCORE || mode == ENC_FN_FOLLOW || mode == ENC_FN_RECORD) {
            expect = SKY_ITEM_SCORE;
        }
        if (typ != expect) {
            code = SKY_RC_BAD_ARG; /* set_mode first, then open matching type */
            break;
        }
        if (!enc_fn_select_idx((int)idx)) {
            code = SKY_RC_FAILED;
            break;
        }
        /* Open = select + start (App long-name fallback uses index play). */
        enc_fn_activate();
        bump_unlocked();
        break;
    }
    case SKY_OP_FOLLOW: {
        if (len < 5) {
            code = SKY_RC_BAD_ARG;
            break;
        }
        uint8_t act = cmd[4];
        if (act == SKY_FOLLOW_STOP) {
            drill_follow_stop();
            bump_unlocked();
            break;
        }
        if (act == SKY_FOLLOW_START) {
            if (len >= 7) {
                uint16_t idx = (uint16_t)cmd[5] | ((uint16_t)cmd[6] << 8);
                if (enc_fn_mode() != ENC_FN_FOLLOW) {
                    enc_fn_set_mode(ENC_FN_FOLLOW);
                }
                if (!enc_fn_select_idx((int)idx)) {
                    code = SKY_RC_FAILED;
                    break;
                }
            } else if (enc_fn_mode() != ENC_FN_FOLLOW) {
                enc_fn_set_mode(ENC_FN_FOLLOW);
            }
            const char *path = enc_fn_list_path(enc_fn_list_idx());
            if (!path || !path[0] || path[0] == '-') {
                code = SKY_RC_FAILED;
                break;
            }
            esp_err_t er = drill_follow_start(path);
            if (er == ESP_ERR_INVALID_ARG) {
                code = SKY_RC_BAD_ARG;
            } else if (er == ESP_ERR_INVALID_STATE) {
                code = SKY_RC_BUSY;
            } else if (er != ESP_OK) {
                code = SKY_RC_FAILED;
            } else {
                bump_unlocked();
            }
            break;
        }
        code = SKY_RC_BAD_ARG;
        break;
    }
    case SKY_OP_WIFI_CONFIG: {
        if (len < 5) {
            code = SKY_RC_BAD_ARG;
            break;
        }
        uint8_t act = cmd[4];
        esp_err_t er = ESP_OK;
        if (act == SKY_WIFI_OFF) {
            er = net_wifi_off();
        } else if (act == SKY_WIFI_AP) {
            er = net_wifi_ap();
        } else if (act == SKY_WIFI_STA) {
            er = net_wifi_sta();
            if (er == ESP_ERR_NOT_FOUND) {
                code = SKY_RC_FAILED;
                break;
            }
        } else if (act == SKY_WIFI_CLEAR) {
            er = net_wifi_clear();
        } else if (act == SKY_WIFI_JOIN) {
            if (len < 7) {
                code = SKY_RC_BAD_ARG;
                break;
            }
            uint8_t sl = cmd[5];
            if (sl == 0 || sl > 32 || len < (size_t)(7 + sl)) {
                code = SKY_RC_BAD_ARG;
                break;
            }
            uint8_t pl = cmd[6 + sl];
            if (pl > 63 || len < (size_t)(7 + sl + pl)) {
                code = SKY_RC_BAD_ARG;
                break;
            }
            char ssid[33] = {0};
            char pass[64] = {0};
            memcpy(ssid, &cmd[6], sl);
            if (pl) {
                memcpy(pass, &cmd[7 + sl], pl);
            }
            er = net_wifi_join(ssid, pass);
        } else {
            code = SKY_RC_BAD_ARG;
            break;
        }
        if (er != ESP_OK && code == SKY_RC_OK) {
            code = SKY_RC_FAILED;
        } else if (er == ESP_OK) {
            bump_unlocked();
        }
        break;
    }
    case SKY_OP_SESSION_TOKEN: {
        if (len < 5) {
            code = SKY_RC_BAD_ARG;
            break;
        }
        uint8_t act = cmd[4];
        if (act == SKY_TOKEN_REVOKE) {
            sky_session_revoke();
            bump_unlocked();
        } else if (act == SKY_TOKEN_ISSUE) {
            if (sky_session_issue() != ESP_OK) {
                code = SKY_RC_FAILED;
            } else {
                bump_unlocked();
            }
        } else {
            code = SKY_RC_BAD_ARG;
        }
        break;
    }
    case SKY_OP_SCORE_FILE: {
        if (len < 5) {
            code = SKY_RC_BAD_ARG;
            break;
        }
        uint8_t act = cmd[4];
        if (act == SKY_SCORE_LIST) {
            uint16_t off = 0;
            if (len >= 7) {
                off = (uint16_t)cmd[5] | ((uint16_t)cmd[6] << 8);
            }
            build_score_list_page(off);
            bump_unlocked();
        } else if (act == SKY_SCORE_PUT_BEGIN) {
            if (len < 10) {
                code = SKY_RC_BAD_ARG;
                break;
            }
            uint32_t sz = (uint32_t)cmd[5] | ((uint32_t)cmd[6] << 8) |
                          ((uint32_t)cmd[7] << 16) | ((uint32_t)cmd[8] << 24);
            uint8_t nl = cmd[9];
            if (nl == 0 || nl > 63 || len < (size_t)(10 + nl) || sz == 0 || sz > 65536u) {
                code = SKY_RC_BAD_ARG;
                break;
            }
            char name[64] = {0};
            memcpy(name, &cmd[10], nl);
            if (!score_name_valid(name)) {
                code = SKY_RC_BAD_ARG;
                break;
            }
            if (s_ble_up_fp) {
                fclose(s_ble_up_fp);
                s_ble_up_fp = NULL;
            }
            (void)mkdir(SD_MOUNT_POINT "/puzi", 0775);
            snprintf(s_ble_up_final, sizeof(s_ble_up_final), "%s/puzi/%s", SD_MOUNT_POINT, name);
            s_ble_up_fp = fopen(SD_MOUNT_POINT "/puzi/.ble_up.tmp", "wb");
            if (!s_ble_up_fp) {
                code = SKY_RC_FAILED;
                break;
            }
            s_ble_up_expect = sz;
            s_ble_up_written = 0;
        } else if (act == SKY_SCORE_PUT_CHUNK) {
            if (!s_ble_up_fp || len < 7) {
                code = SKY_RC_BAD_ARG;
                break;
            }
            uint16_t off = (uint16_t)cmd[5] | ((uint16_t)cmd[6] << 8);
            size_t clen = len - 7;
            if ((uint32_t)off != s_ble_up_written || s_ble_up_written + clen > s_ble_up_expect) {
                code = SKY_RC_BAD_ARG;
                break;
            }
            if (clen > 0 && fwrite(&cmd[7], 1, clen, s_ble_up_fp) != clen) {
                fclose(s_ble_up_fp);
                s_ble_up_fp = NULL;
                code = SKY_RC_FAILED;
                break;
            }
            s_ble_up_written += (uint32_t)clen;
        } else if (act == SKY_SCORE_PUT_COMMIT) {
            if (!s_ble_up_fp || s_ble_up_written != s_ble_up_expect) {
                if (s_ble_up_fp) {
                    fclose(s_ble_up_fp);
                    s_ble_up_fp = NULL;
                }
                code = SKY_RC_FAILED;
                break;
            }
            fclose(s_ble_up_fp);
            s_ble_up_fp = NULL;
            (void)unlink(s_ble_up_final);
            if (rename(SD_MOUNT_POINT "/puzi/.ble_up.tmp", s_ble_up_final) != 0) {
                code = SKY_RC_FAILED;
                break;
            }
            enc_fn_rescan_current();
            build_score_list_page(0);
            bump_unlocked();
        } else if (act == SKY_SCORE_DELETE) {
            if (len < 6) {
                code = SKY_RC_BAD_ARG;
                break;
            }
            uint8_t nl = cmd[5];
            if (nl == 0 || nl > 63 || len < (size_t)(6 + nl)) {
                code = SKY_RC_BAD_ARG;
                break;
            }
            char name[64] = {0};
            memcpy(name, &cmd[6], nl);
            if (!score_name_valid(name)) {
                code = SKY_RC_BAD_ARG;
                break;
            }
            char path[160];
            snprintf(path, sizeof(path), "%s/puzi/%s", SD_MOUNT_POINT, name);
            if (unlink(path) != 0) {
                code = SKY_RC_FAILED;
                break;
            }
            enc_fn_rescan_current();
            build_score_list_page(0);
            bump_unlocked();
        } else if (act == SKY_SCORE_RENAME) {
            if (len < 7) {
                code = SKY_RC_BAD_ARG;
                break;
            }
            uint8_t fl = cmd[5];
            if (fl == 0 || fl > 63 || len < (size_t)(7 + fl)) {
                code = SKY_RC_BAD_ARG;
                break;
            }
            uint8_t tl = cmd[6 + fl];
            if (tl == 0 || tl > 63 || len < (size_t)(7 + fl + tl)) {
                code = SKY_RC_BAD_ARG;
                break;
            }
            char from[64] = {0}, to[64] = {0};
            memcpy(from, &cmd[6], fl);
            memcpy(to, &cmd[7 + fl], tl);
            if (!score_name_valid(from) || !score_name_valid(to)) {
                code = SKY_RC_BAD_ARG;
                break;
            }
            char pf[160], pt[160];
            snprintf(pf, sizeof(pf), "%s/puzi/%s", SD_MOUNT_POINT, from);
            snprintf(pt, sizeof(pt), "%s/puzi/%s", SD_MOUNT_POINT, to);
            (void)unlink(pt);
            if (rename(pf, pt) != 0) {
                code = SKY_RC_FAILED;
                break;
            }
            enc_fn_rescan_current();
            build_score_list_page(0);
            bump_unlocked();
        } else {
            code = SKY_RC_BAD_ARG;
        }
        break;
    }
    case SKY_OP_OPEN_NAME: {
        if (len < 6) {
            code = SKY_RC_BAD_ARG;
            break;
        }
        uint8_t act = cmd[4];
        uint8_t nl = cmd[5];
        if (nl == 0 || nl > SKY_NAME_MAX_BYTES || len < (size_t)(6 + nl)) {
            code = SKY_RC_BAD_ARG;
            break;
        }
        char name[SKY_NAME_MAX_BYTES + 1];
        memset(name, 0, sizeof(name));
        memcpy(name, &cmd[6], nl);
        if (act == SKY_NAME_PLAY_SCORE) {
            if (enc_fn_mode() != ENC_FN_SCORE) {
                enc_fn_set_mode(ENC_FN_SCORE);
            }
            if (!enc_fn_select_name(name)) {
                code = SKY_RC_FAILED;
                break;
            }
            enc_fn_activate();
            for (int i = 0; i < 40 && score_is_opening(); ++i) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (score_note_count() <= 0) {
                code = SKY_RC_FAILED; /* empty / bad format / missing file */
                break;
            }
            bump_unlocked();
        } else if (act == SKY_NAME_FOLLOW_SCORE) {
            if (enc_fn_mode() != ENC_FN_FOLLOW) {
                enc_fn_set_mode(ENC_FN_FOLLOW);
            }
            if (!enc_fn_select_name(name)) {
                code = SKY_RC_FAILED;
                break;
            }
            const char *path = enc_fn_list_path(enc_fn_list_idx());
            if (!path || !path[0] || path[0] == '-') {
                code = SKY_RC_FAILED;
                break;
            }
            sd_music_cmd("stop");
            score_cmd_stop();
            drill_follow_stop();
            drill_follow_toggle(path);
            bump_unlocked();
        } else if (act == SKY_NAME_PLAY_MUSIC) {
            if (enc_fn_mode() != ENC_FN_MUSIC) {
                enc_fn_set_mode(ENC_FN_MUSIC);
            }
            if (!enc_fn_select_name(name)) {
                code = SKY_RC_FAILED;
                break;
            }
            enc_fn_activate();
            bump_unlocked();
        } else {
            code = SKY_RC_BAD_ARG;
        }
        break;
    }
    default:
        code = SKY_RC_UNSUPPORTED;
        break;
    }
    fill_result(result, op, req, code);
    unlock();
    return ESP_OK;
}

void sky_proto_dump_state(void)
{
    sky_state_v1_t st;
    sky_proto_fill_state(&st);
    printf("sky_state ver=%u seq=%u mode=%u map=%u vol=%u tr=%d tempo=%u transport=%u\n", st.ver,
           st.state_seq, st.mode, st.map, st.volume, (int)st.transpose, st.tempo_pct, st.transport);
    printf("  keys=0x%04X expect=0x%04X follow=%u at=%u/%u bat=%u%% %umV lvl=%u chg=0x%02X\n",
           st.key_mask, st.expect_mask, st.follow_state, st.follow_at, st.follow_total, st.bat_pct,
           st.bat_mv, st.bat_level, st.chg_flags);
}

bool sky_proto_handle_line(const char *line)
{
    if (!line) {
        return false;
    }
    if (strcmp(line, "sky") == 0 || strcmp(line, "sky snap") == 0) {
        sky_proto_dump_state();
        return true;
    }
    if (strcmp(line, "sky token") == 0) {
        char hex[40];
        if (sky_session_hex(hex, sizeof(hex))) {
            printf("sky_token %s ttl=%u wifi=%s ip=%s\n", hex, (unsigned)sky_session_ttl_remaining(),
                   net_wifi_mode_str(), net_wifi_ip());
        } else {
            printf("sky_token (none) wifi=%s ip=%s\n", net_wifi_mode_str(), net_wifi_ip());
        }
        return true;
    }
    if (strncmp(line, "sky cmd ", 8) == 0) {
        /* sky cmd <hex bytes...>  e.g. sky cmd 01 01 01 00 */
        uint8_t buf[SKY_CMD_MAX_SIZE];
        size_t n = 0;
        const char *p = line + 8;
        while (*p && n < sizeof(buf)) {
            while (*p == ' ') {
                p++;
            }
            if (!*p) {
                break;
            }
            unsigned v = 0;
            if (sscanf(p, "%02x", &v) != 1 && sscanf(p, "%x", &v) != 1) {
                break;
            }
            buf[n++] = (uint8_t)v;
            while (*p && *p != ' ') {
                p++;
            }
        }
        sky_result_v1_t r;
        if (sky_proto_exec(buf, n, &r) != ESP_OK) {
            printf("sky cmd parse fail\n");
            return true;
        }
        printf("sky_result op=0x%02X req=%u rc=%u seq=%u\n", r.opcode, r.request_id, r.result_code,
               r.state_seq);
        if (r.opcode == SKY_OP_SESSION_TOKEN && r.result_code == SKY_RC_OK) {
            char hex[40];
            if (sky_session_hex(hex, sizeof(hex))) {
                printf("sky_token %s ttl=%u\n", hex, (unsigned)sky_session_ttl_remaining());
            } else {
                printf("sky_token (revoked)\n");
            }
        }
        if (r.opcode == SKY_OP_GET_SNAPSHOT || r.result_code == SKY_RC_OK) {
            sky_proto_dump_state();
        }
        return true;
    }
    return false;
}
