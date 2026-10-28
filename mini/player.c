/* ===========================================================================
 *  BoltOS Mini  -  mini/player.c
 *
 *  YouTube, played on the Pi. A watch page asks YouTube's player API what
 *  the video is and where it lives, as YouTube's Android app does -- that
 *  client is handed plain stream URLs, with no signature to work out. Of
 *  what it offers, format 18 is the one to play: 360p H.264 Baseline with
 *  AAC, both in one MP4 with its index at the front, so it can be streamed
 *  and needs no B-frames or CABAC.
 * ===========================================================================*/
#include <stdint.h>
#include "mini.h"
#include "web.h"
#include "gfx.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

#define YT_UA "User-Agent: com.google.android.youtube/20.10.38 (Linux; U; Android 11) gzip\r\n"

static struct {
    int  state;                     /* 0 idle, 1 loading, 2 playing, 3 paused, -1 error */
    char msg[160];
    char url[2048];                 /* format 18                                     */
    int  len_ms, pos_ms;
} P;

static void commas(long long v, char *out, int cap) {
    char d[32]; int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 30);
    int o = 0;
    for (int i = n - 1; i >= 0 && o < cap - 2; i--) {
        out[o++] = d[i];
        if (i && i % 3 == 0) out[o++] = ',';
    }
    out[o] = 0;
}

int player_open(const char *id, char *title, int title_cap, char *meta, int meta_cap) {
    memset(&P, 0, sizeof P);
    P.state = 1;
    snprintf(P.msg, sizeof P.msg, "Asking YouTube for the video\x85");
    char body[600];
    int bl = snprintf(body, sizeof body,
        "{\"context\":{\"client\":{\"clientName\":\"ANDROID\",\"clientVersion\":\"20.10.38\","
        "\"androidSdkVersion\":30,\"osName\":\"Android\",\"osVersion\":\"11\",\"hl\":\"en\",\"gl\":\"US\"}},"
        "\"videoId\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true}", id);
    char *buf = kmalloc(1024 * 1024);
    if (!buf) { P.state = -1; snprintf(P.msg, sizeof P.msg, "Out of memory"); return -1; }
    int code = 0;
    int n = web_request("POST", "https://www.youtube.com/youtubei/v1/player?prettyPrint=false",
                        YT_UA "Content-Type: application/json\r\n", body, bl, buf, 1024 * 1024, &code, 0, 0);
    jarena *ar = 0;
    jv *root = n > 0 ? json_parse(buf, n, &ar) : 0;
    if (!root) {
        P.state = -1;
        snprintf(P.msg, sizeof P.msg, n < 0 ? "YouTube didn't answer" : "YouTube's answer couldn't be read");
        kfree(buf);
        return -1;
    }
    jv *vd = json_get(root, "videoDetails");
    const char *t = json_s(json_get(vd, "title"));
    utf8_to_cp1252(t, (int)strlen(t), title, title_cap);
    char views[32]; commas(json_i(json_get(vd, "viewCount")), views, sizeof views);
    const char *author = json_s(json_get(vd, "author"));
    char au[120]; utf8_to_cp1252(author, (int)strlen(author), au, sizeof au);
    P.len_ms = (int)json_i(json_get(vd, "lengthSeconds")) * 1000;
    snprintf(meta, (size_t)meta_cap, "%s  \xB7  %s views", au, views);

    const char *status = json_s(json_path(root, "playabilityStatus.status"));
    if (strcmp(status, "OK")) {
        const char *why = json_s(json_path(root, "playabilityStatus.reason"));
        P.state = -1;
        utf8_to_cp1252(why[0] ? why : "YouTube won't play this one here", (int)strlen(why[0] ? why : "YouTube won't play this one here"), P.msg, sizeof P.msg);
    } else {
        jv *fm = json_path(root, "streamingData.formats");
        for (jv *f = fm ? fm->kid : 0; f; f = f->next)
            if (json_i(json_get(f, "itag")) == 18) snprintf(P.url, sizeof P.url, "%s", json_s(json_get(f, "url")));
        if (!P.url[0]) {
            P.state = -1;
            snprintf(P.msg, sizeof P.msg, json_get(json_get(root, "streamingData"), "hlsManifestUrl") ?
                     "Live streams don't play yet" : "There's no 360p stream of this video");
        } else {
            P.state = -1;
            snprintf(P.msg, sizeof P.msg, "Found the 360p stream; the decoder is next");
        }
    }
    kprintf("[yt] %s: %s, %d s, %s\n", id, status, P.len_ms / 1000, P.url[0] ? "format 18" : "no format 18");
    json_free(ar);
    kfree(buf);
    return P.state < 0 ? -1 : 0;
}

void player_close(void) { memset(&P, 0, sizeof P); }
int  player_tick(void) { return 0; }
const surface_t *player_frame(void) { return 0; }
int  player_status(char *msg, int cap) { snprintf(msg, (size_t)cap, "%s", P.msg); return P.state; }
void player_toggle(void) { }
void player_seek(int secs) { (void)secs; }
int  player_pos_ms(void) { return P.pos_ms; }
int  player_len_ms(void) { return P.len_ms; }
