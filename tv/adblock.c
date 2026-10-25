/* ===========================================================================
 *  BoltOS TV  -  tv/adblock.c
 *
 *  An ad blocker that is part of the television rather than an extension to
 *  one program on it. Three layers:
 *
 *   1. Names. Every lookup the set makes goes through net/dns.c, which asks
 *      dns_blocked() first (tv/stubs.c -> here). A name on the list, or any
 *      name under one -- securepubads.g.doubleclick.net is under
 *      doubleclick.net -- is refused before a packet leaves, so it applies
 *      to anything that will ever use the network, not just the browser.
 *   2. Addresses. The browser checks each URL it is about to fetch against
 *      the same names and a short list of path patterns that only ad
 *      servers use (/pagead/, /adserver, ...).
 *   3. Elements. Before a page is laid out, elements whose id or class say
 *      they are an ad slot are taken out of the tree, so the hole an ad
 *      would have filled closes up instead of showing as a blank box.
 *
 *  The list is three categories: advertising networks, analytics and
 *  tracking, and the telemetry smart TVs send home about what you watch.
 *  It is deliberately conservative -- nothing a page needs to work is on it.
 * ===========================================================================*/
#include <stdint.h>
#include "tv.h"
#include "io.h"
#include "string.h"
#include "stdio.h"
#include "kprintf.h"
#include "kheap.h"

static const char *const LIST[] = {
    /* ---- advertising -------------------------------------------------- */
    "doubleclick.net", "googlesyndication.com", "googleadservices.com",
    "googletagservices.com", "adservice.google.com", "pagead2.googlesyndication.com",
    "ad.doubleclick.net", "adsense.google.com", "partner.googleadservices.com",
    "adnxs.com", "adsrvr.org", "advertising.com", "criteo.com", "criteo.net",
    "taboola.com", "taboolasyndication.com", "outbrain.com", "outbrainimg.com",
    "amazon-adsystem.com", "aax.amazon-adsystem.com", "adsafeprotected.com",
    "rubiconproject.com", "pubmatic.com", "openx.net", "openx.com", "casalemedia.com",
    "indexww.com", "smartadserver.com", "adform.net", "adformdsp.net", "bidswitch.net",
    "33across.com", "sharethrough.com", "triplelift.com", "3lift.com", "yieldmo.com",
    "teads.tv", "media.net", "contextweb.com", "sovrn.com", "lijit.com", "gumgum.com",
    "zedo.com", "revcontent.com", "mgid.com", "adblade.com", "propellerads.com",
    "popads.net", "popcash.net", "exoclick.com", "juicyads.com", "trafficjunky.net",
    "adcash.com", "adroll.com", "serving-sys.com", "flashtalking.com", "innovid.com",
    "spotxchange.com", "spotx.tv", "springserve.com", "fwmrm.net", "freewheel.tv",
    "adtechus.com", "yieldlab.net", "adition.com", "ligatus.com", "plista.com",
    "adfox.ru", "an.yandex.ru", "pos.baidu.com", "cpro.baidu.com", "ads.yahoo.com",
    "gemini.yahoo.com", "adtech.yahoo.com", "ads.linkedin.com", "ads-twitter.com",
    "static.ads-twitter.com", "ads.tiktok.com", "ads.pinterest.com", "adsymptotic.com",
    "mathtag.com", "turn.com", "media6degrees.com",
    "adzerk.net", "carbonads.com", "buysellads.com", "buysellads.net", "infolinks.com",
    "vibrantmedia.com", "kontera.com", "intellitxt.com", "undertone.com", "conversantmedia.com",
    "dotomi.com", "valueclick.com", "valueclick.net", "tribalfusion.com", "exponential.com",
    "yieldmanager.com", "yieldmanager.net", "rightmedia.com", "atdmt.com", "adbrite.com",
    "adsonar.com", "adhigh.net", "adkernel.com", "admixer.net", "adocean.pl", "adriver.ru",
    "adskeeper.com", "adsterra.com", "adtelligent.com", "adthrive.com", "advertserve.com",
    "affec.tv", "amgdgt.com", "appnexus.com", "bidr.io", "bidtheatre.com", "brightroll.com",
    "chitika.com", "chitika.net", "clickadu.com", "connatix.com", "districtm.io",
    "e-planning.net", "emxdgt.com", "engageya.com", "ezoic.net", "ezodn.com", "fastclick.net",
    "go2cloud.org", "hilltopads.net", "imasdk.googleapis.com", "inmobi.com", "ironsrc.com",
    "kargo.com", "lkqd.net", "loopme.com", "mopub.com", "nativo.com",
    "nexac.com", "onclickads.net", "onetag-sys.com", "playwire.com",
    "pubnative.net", "pulsepoint.com", "quantcount.com", "richaudience.com", "rlcdn.com",
    "rtbhouse.com", "s-onetag.com", "seedtag.com", "smaato.net", "smartyads.com", "sonobi.com",
    "stackadapt.com", "tapjoy.com", "teads.com", "themoneytizer.com",
    "tremorhub.com", "tremormedia.com", "unrulymedia.com", "verve.com", "vidazoo.com",
    "vungle.com", "yieldlove.com", "yieldone.com", "zemanta.com", "adsco.re", "adcolony.com",
    "applovin.com", "unityads.unity3d.com", "chartboost.com", "startappservice.com",
    /* ---- analytics and tracking ---------------------------------------- */
    "google-analytics.com", "ssl.google-analytics.com", "analytics.google.com",
    "googletagmanager.com", "app-measurement.com", "scorecardresearch.com", "quantserve.com",
    "moatads.com", "hotjar.com", "hotjar.io", "mixpanel.com", "api.segment.io",
    "cdn.segment.com", "fullstory.com", "mouseflow.com", "crazyegg.com", "clicktale.net",
    "nr-data.net", "js-agent.newrelic.com", "chartbeat.com", "chartbeat.net",
    "srv.pixel.parsely.com", "connect.facebook.net", "pixel.facebook.com", "an.facebook.com",
    "analytics.twitter.com", "px.ads.linkedin.com", "snap.licdn.com", "bat.bing.com",
    "analytics.yahoo.com", "demdex.net", "omtrdc.net", "2o7.net", "everesttech.net",
    "krxd.net", "bluekai.com", "exelator.com", "tapad.com", "agkn.com", "mc.yandex.ru",
    "hm.baidu.com", "analytics.tiktok.com", "ct.pinterest.com", "log.pinterest.com",
    "pixel.redditmedia.com", "alb.reddit.com", "q.quora.com", "appsflyer.com", "adjust.com",
    "kochava.com", "api.amplitude.com", "heap.io", "heapanalytics.com",
    "logx.optimizely.com", "kissmetrics.com", "woopra.com", "statcounter.com",
    "getclicky.com", "addthis.com", "addthisedge.com", "sharethis.com",
    "bounceexchange.com", "bouncex.net", "pippio.com", 
    "eyeota.net", "lotame.com", "crwdcntrl.net", "zeotap.com", "id5-sync.com", "adsrvr.com",
    "permutive.com", "permutive.app", "trackcmp.net",
    "tealiumiq.com", "tags.tiqcdn.com", "ensighten.com", "siteintercept.qualtrics.com",
    "mookie1.com", "imrworldwide.com", "effectivemeasure.net",
    "gemius.pl", "hit.gemius.pl", "webtrends.com", "webtrendslive.com", "coremetrics.com",
    "mxpnl.com", "luckyorange.com", "luckyorange.net", "inspectlet.com", "smartlook.com",
    "clarity.ms", "c.clarity.ms", "visualwebsiteoptimizer.com", 
    "doubleverify.com", "iasds01.com", "adlightning.com", "confiant-integrations.net",
    
    /* ---- smart-TV telemetry ------------------------------------------- */
    "samsungads.com", "ads.samsung.com", "samsungacr.com", "log-config.samsungacr.com",
    "lgads.tv", "lgsmartad.com", "ad.lgappstv.com", "info.lgsmartad.com", 
    "ads.roku.com", "logs.roku.com", "cooper.logs.roku.com", "giga.logs.roku.com",
    "tvinteractive.tv", "alphonso.tv", "samba.tv", "inscape.tv", 
    "ads.vizio.com", "smartclip.net",
    "device-metrics-us.amazon.com", "device-metrics-us-2.amazon.com", "mads.amazon.com",
};
#define NLIST ((int)(sizeof LIST / sizeof LIST[0]))

/* ---- the set: open addressing over FNV-1a ------------------------------- */
#define HSIZE 1024                     /* > 2x the list, power of two */
static const char *htab[HSIZE];
static int hbuilt;

static uint32_t fnv(const char *s, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        h = (h ^ c) * 16777619u;
    }
    return h;
}

static int name_eq(const char *a, int n, const char *b) {
    for (int i = 0; i < n; i++) {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != b[i]) return 0;
    }
    return b[n] == 0;
}

static void build(void) {
    for (int i = 0; i < NLIST; i++) {
        uint32_t h = fnv(LIST[i], (int)strlen(LIST[i])) & (HSIZE - 1);
        while (htab[h] && strcmp(htab[h], LIST[i]) != 0) h = (h + 1) & (HSIZE - 1);
        htab[h] = LIST[i];
    }
    hbuilt = 1;
}

static int in_list(const char *s, int n) {
    uint32_t h = fnv(s, n) & (HSIZE - 1);
    while (htab[h]) {
        if (name_eq(s, n, htab[h])) return 1;
        h = (h + 1) & (HSIZE - 1);
    }
    return 0;
}

/* The name itself, then every parent above it that still has a dot. */
static int match(const char *host, int n) {
    if (!hbuilt) build();
    while (n > 0 && host[n - 1] == '.') n--;          /* trailing root dot */
    const char *p = host;
    for (;;) {
        if (in_list(p, n)) return 1;
        const char *dot = memchr(p, '.', (size_t)n);
        if (!dot) return 0;
        n -= (int)(dot + 1 - p);
        p = dot + 1;
        if (!memchr(p, '.', (size_t)n)) return 0;    /* never test a bare TLD */
    }
}

/* ---- statistics --------------------------------------------------------- */
static volatile uint32_t n_dns, n_web, n_hidden;

#define RECENT 12
static struct { char host[64]; uint32_t count; } recent[RECENT];
static int nrecent;

static void note(const char *host, int n) {
    if (n > 63) n = 63;
    uint64_t f = irq_save();
    int at = -1;
    for (int i = 0; i < nrecent; i++)
        if ((int)strlen(recent[i].host) == n && memcmp(recent[i].host, host, (size_t)n) == 0) { at = i; break; }
    uint32_t count = 1;
    if (at >= 0) {
        count = recent[at].count + 1;
        for (int i = at; i > 0; i--) recent[i] = recent[i - 1];
    } else {
        int last = nrecent < RECENT ? nrecent++ : RECENT - 1;
        for (int i = last; i > 0; i--) recent[i] = recent[i - 1];
    }
    memcpy(recent[0].host, host, (size_t)n);
    recent[0].host[n] = 0;
    recent[0].count = count;
    irq_restore(f);
}

int adblock_host_blocked(const char *host) {
    if (!host || !host[0]) return 0;
    int n = (int)strlen(host);
    if (!match(host, n)) return 0;
    n_dns++;
    note(host, n);
    return 1;
}

static const char *const PATHS[] = {
    "/pagead/", "/adserver", "/adframe", "/ads.js", "/ads/", "/adsbygoogle",
    "/advertisement", "/doubleclick/", "/prebid", "/gpt.js", "/ad_banner", "/banner_ad",
};

int adblock_url_blocked(const char *url) {
    if (!url) return 0;
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    int n = 0;
    while (h[n] && h[n] != '/' && h[n] != ':' && h[n] != '?' && h[n] != '#') n++;
    if (n > 0 && match(h, n)) { n_web++; note(h, n); return 1; }
    const char *path = h + n;
    for (unsigned i = 0; i < sizeof PATHS / sizeof PATHS[0]; i++)
        if (strstr(path, PATHS[i])) { n_web++; note(h, n); return 1; }
    return 0;
}

/* ---- element hiding ------------------------------------------------------ */
static const char *const EXACT[] = {
    "ad", "ads", "advert", "adverts", "advertisement", "advertising", "adsbygoogle",
    "adbox", "adunit", "adslot", "adspace", "adzone", "adwrap", "sponsored", "sponsor",
    "promoted", "banner-ad", "dfp", "taboola", "outbrain", "OUTBRAIN", "trc_related_container",
};
static const char *const PREFIX[] = {
    "ad-", "ad_", "ads-", "ads_", "advert", "adsense", "adslot", "adunit", "google-ad",
    "gpt-ad", "dfp-", "sponsor", "taboola", "outbrain", "native-ad", "dfp_",
};

static int token_is_ad(const char *t, int n) {
    for (unsigned i = 0; i < sizeof EXACT / sizeof EXACT[0]; i++)
        if ((int)strlen(EXACT[i]) == n && strncasecmp(t, EXACT[i], (size_t)n) == 0) return 1;
    for (unsigned i = 0; i < sizeof PREFIX / sizeof PREFIX[0]; i++) {
        int pl = (int)strlen(PREFIX[i]);
        if (n > pl && strncasecmp(t, PREFIX[i], (size_t)pl) == 0) return 1;
    }
    return 0;
}

static int words_have_ad(const char *s) {
    if (!s) return 0;
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;
        const char *w = s;
        while (*s && *s != ' ' && *s != '\t' && *s != '\n') s++;
        if (s > w && token_is_ad(w, (int)(s - w))) return 1;
    }
    return 0;
}

int adblock_element_hidden(const char *tag, const char *id, const char *cls) {
    if (tag && (strcmp(tag, "amp-ad") == 0 || strcmp(tag, "amp-embed") == 0 ||
                strcmp(tag, "amp-sticky-ad") == 0))
        return 1;
    return words_have_ad(id) || words_have_ad(cls);
}

void adblock_note_hidden(int n) { n_hidden += (uint32_t)n; }

int adblock_list_size(void)       { return NLIST; }
uint32_t adblock_blocked_dns(void) { return n_dns; }
uint32_t adblock_blocked_web(void) { return n_web; }
uint32_t adblock_hidden_elements(void) { return n_hidden; }
uint32_t adblock_blocked_total(void) { return n_dns + n_web + n_hidden; }

int adblock_recent(int i, const char **host, uint32_t *count) {
    if (i < 0 || i >= nrecent) return 0;
    *host = recent[i].host;
    *count = recent[i].count;
    return 1;
}

/* ===========================================================================
 *  The Ad Blocker screen: is it on, what has it stopped, and how.
 * ===========================================================================*/
static int ab_focus;                       /* 0 the switch, 1 run a test */
static glasscache_t gc_main, gc_side;
static fetch_t *tests[4];
static uint64_t test_t0;

static const char *const TEST_URLS[4] = {
    "https://securepubads.g.doubleclick.net/tag/js/gpt.js",
    "https://www.google-analytics.com/analytics.js",
    "https://cdn.taboola.com/libtrc/loader.js",
    "https://log-config.samsungacr.com/",
};

static void run_test(void) {
    for (int i = 0; i < 4; i++) {
        if (tests[i]) { netsvc_cancel(tests[i]); tests[i] = 0; }
        fetch_t *f = (fetch_t *)kmalloc(sizeof *f);
        char *b = (char *)kmalloc(256);
        if (!f || !b) { if (f) kfree(f); if (b) kfree(b); continue; }
        memset(f, 0, sizeof *f);
        strncpy(f->url, TEST_URLS[i], sizeof f->url);
        f->buf = b; f->cap = 256;
        if (netsvc_fetch(f) == 0) tests[i] = f;
        else { kfree(b); kfree(f); }
    }
    test_t0 = tv_now_ms();
}

static void ab_key(tvkey_t k) {
    switch (k.k) {
    case K_UP: case K_LEFT:   if (ab_focus > 0) { ab_focus--; sfx_move(); } break;
    case K_DOWN: case K_RIGHT: if (ab_focus < 1) { ab_focus++; sfx_move(); } break;
    case K_OK:
        sfx_select();
        if (ab_focus == 0) {
            tvs.adblock = !tvs.adblock;
            tv_toast(tvs.adblock ? "Ad Blocker on" : "Ad Blocker off",
                     tvs.adblock ? "Ads and trackers are blocked for everything on this TV." : 0);
        } else run_test();
        break;
    case K_BACK: sfx_back(); tv_pop(); break;
    }
}

static void ab_pointer(int x, int y, int click, int wheel) {
    (void)wheel;
    int mx = S(56) + S(48), my = S(56);
    if (ui_hit(x, y, mx, my + S(214), S(860), S(100))) { ab_focus = 0; if (click) ab_key((tvkey_t){ K_OK, 0 }); }
    if (ui_hit(x, y, mx, my + S(326), S(860), S(100))) { ab_focus = 1; if (click) ab_key((tvkey_t){ K_OK, 0 }); }
}

static void ab_tick(uint64_t now) {
    static uint64_t last;
    if (now - last > 500 || tv_view_t() < 1.0f) { last = now; tv_redraw(); }
    if (test_t0) {                         /* the test: report once all four are back */
        int done = 0, blocked = 0, n = 0;
        for (int i = 0; i < 4; i++) {
            if (!tests[i]) continue;
            n++;
            if (tests[i]->state == FETCH_DONE || tests[i]->state == FETCH_FAILED) {
                done++;
                if (tests[i]->status == -8) blocked++;
            }
        }
        if (n && done == n) {
            char d[96];
            snprintf(d, sizeof d, "%d of %d known trackers were stopped before a packet left the TV.", blocked, n);
            tv_toast(blocked == n ? "Ad Blocker is working" : "Some trackers got through", d);
            for (int i = 0; i < 4; i++) if (tests[i]) { netsvc_cancel(tests[i]); tests[i] = 0; }
            test_t0 = 0;
        }
    }
}

static void stat_card(int x, int y, int w, int h, const char *num, const char *label, uint32_t accent, int a) {
    gfx_rrect(x, y, w, h, S(26), 0xFFFFFF, a * 22 / 255);
    gfx_rrect_stroke2(x, y, w, h, S(26), 0xFFFFFF, a * 60 / 255, a * 10 / 255);
    gfx_rrect(x + S(26), y + S(28), S(8), S(42), S(4), accent, a);
    ui_text(FONT_SEMIBOLD, S(52), x + S(52), y + S(50), num, 0xFFFFFF, a, 0);
    ui_text(FONT_REGULAR, S(22), x + S(28), y + S(110), label, 0xB9C0CC, a, 0);
}

static void ab_draw(uint64_t now) {
    (void)now;
    float vt = tv_view_t();
    int a = (int)(vt * 255), slide = (int)((1.0f - vt) * S(40));
    int x = S(56) - slide, y = S(56), w = S(960), h = tv_h - S(112);
    ui_glass(&gc_main, x, y, w, h, S(40), 160, a);
    art_tile(ART_SHIELD, x + S(44), y + S(40), S(96), a);
    ui_text(FONT_SEMIBOLD, S(52), x + S(166), y + S(76), "Ad Blocker", 0xFFFFFF, a, 0);
    ui_text(FONT_REGULAR, S(24), x + S(168), y + S(122),
            tvs.adblock ? "On for every app on this TV" : "Off", tvs.adblock ? 0x7CE3A6 : 0xFFB4A8, a, 0);

    int rx = x + S(48), rw = w - S(96);
    int ry0 = y + S(214), ry1 = y + S(326);
    ui_focus(rx - S(16), (ab_focus ? ry1 : ry0), rw + S(32), S(100), S(26), a);
    uint32_t i0 = ab_focus == 0 ? 0x101318 : 0xFFFFFF, i1 = ab_focus == 1 ? 0x101318 : 0xFFFFFF;
    ui_text(FONT_MEDIUM, S(30), rx + S(10), ry0 + S(50), "Block ads and trackers", i0, a, 0);
    if (ab_focus == 0) {
        int sw = S(76), sh = S(44), sx = rx + rw - sw - S(10), cy = ry0 + S(50);
        gfx_rrect(sx, cy - sh / 2, sw, sh, sh / 2, tvs.adblock ? 0x34C759 : 0x101318, tvs.adblock ? 255 : 50);
        gfx_circle((float)(tvs.adblock ? sx + sw - sh / 2 : sx + sh / 2), (float)cy, (float)(sh / 2 - S(4)), 0xFFFFFF, 255);
    } else ui_switch(rx + rw - S(86), ry0 + S(50), tvs.adblock, 0);
    ui_text(FONT_MEDIUM, S(30), rx + S(10), ry1 + S(50), "Run a test", i1, a, 0);
    ui_text(FONT_REGULAR, S(22), rx + rw - S(10), ry1 + S(50), "Ask for four known trackers", ab_focus == 1 ? 0x3A404C : 0xAEB5C2, a, 2);

    /* the counters */
    char n0[24], n1[24], n2[24];
    fmt_int(n0, sizeof n0, (long)adblock_blocked_dns());
    fmt_int(n1, sizeof n1, (long)adblock_blocked_web());
    fmt_int(n2, sizeof n2, (long)adblock_hidden_elements());
    int cw = (rw - S(40)) / 3, cy = y + S(470);
    stat_card(rx, cy, cw, S(150), n0, "lookups refused", 0x34C759, a);
    stat_card(rx + cw + S(20), cy, cw, S(150), n1, "addresses skipped", 0x0A84FF, a);
    stat_card(rx + 2 * (cw + S(20)), cy, cw, S(150), n2, "ad slots removed", 0xFF9F0A, a);

    char how[512];
    snprintf(how, sizeof how,
             "Every name this TV looks up is checked against %d advertising, tracking and smart-TV "
             "telemetry networks, and everything under them, before a single packet is sent. Nothing on "
             "the list is ever contacted, whichever app asked. The browser also skips ad scripts and "
             "pictures, and takes the empty ad slots out of every page so the text closes up.",
             adblock_list_size());
    ui_wrap(FONT_REGULAR, S(24), rx, y + S(668), rw, S(36), 7, how, 0xC9CED8, a);

    /* recently blocked */
    int sx = S(56) + S(960) + S(32) + slide, sw = tv_w - sx - S(56);
    ui_glass(&gc_side, sx, y, sw, h, S(40), 160, a);
    ui_text(FONT_SEMIBOLD, S(34), sx + S(40), y + S(62), "Recently blocked", 0xFFFFFF, a, 0);
    const char *host; uint32_t cnt;
    int ly = y + S(130);
    if (!adblock_recent(0, &host, &cnt)) {
        ui_wrap(FONT_REGULAR, S(22), sx + S(40), ly, sw - S(80), S(32), 4,
                "Nothing yet. Open the browser, or run the test, and what gets stopped shows up here.", 0xAEB5C2, a);
    }
    for (int i = 0; adblock_recent(i, &host, &cnt) && ly < y + h - S(60); i++) {
        art_glyph(GL_SHIELD, sx + S(54), ly + S(24), S(26), 0x34C759, a);
        font_draw_fit(FONT_MEDIUM, S(22), sx + S(86), ly + S(32), host, sw - S(200), 0xFFFFFF, a);
        char c[16]; snprintf(c, sizeof c, "\xD7%u", cnt);
        ui_text(FONT_REGULAR, S(20), sx + sw - S(40), ly + S(24), c, 0x9AA2B0, a, 2);
        ly += S(62);
    }
}

const view_t view_adblock = {
    .name = "adblock", .backdrop = BG_FILM_DIM,
    .key = ab_key, .pointer = ab_pointer, .draw = ab_draw, .tick = ab_tick,
};
