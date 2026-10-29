/* ===========================================================================
 *  BoltOS Local AI  -  app.c
 *
 *  The app: a chat with a language model that runs on the Pi itself, and a
 *  page of models to choose from (Tab goes between them).
 *
 *  The conversation stays in the model's cache from one question to the
 *  next, so a question costs only its own tokens. The system prompt goes in
 *  as soon as a model is loaded, before anyone asks anything. When the
 *  cache is full, the conversation starts over from the system prompt.
 *
 *  Web search (Exa, mcp.c) is Auto, Always or Off. Auto searches when the
 *  question asks for something a model trained a while ago can't know --
 *  the news, the weather, today, the latest version, "look it up" -- since
 *  models this small don't reliably decide that themselves. The results go
 *  in front of the question, and the sources are listed above the answer.
 * ===========================================================================*/
#include "ai.h"
#include "web.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

enum { V_CHAT, V_MODELS };
enum { WEB_AUTO, WEB_ALWAYS, WEB_OFF };
static const char *const web_names[3] = { "Auto", "Always", "Off" };
enum { R_USER, R_AI, R_WEB, R_NOTE };

typedef struct {
    int   role;
    char *u8; int len, cap;              /* what was said, UTF-8                    */
    char *shown;                         /* the same for the screen (cp1252)        */
    int   shown_len;
    float rate;                          /* an answer: tokens a second              */
    int   ntok;
} msg_t;

#define MAX_MSG 80
static msg_t msgs[MAX_MSG];
static int nmsg;

static struct {
    int view, web, sel;                  /* the models page: the selected row       */
    int confirm;                         /* Backspace pressed once on this row      */
    char in[1024]; int in_len;           /* what's being typed (cp1252)             */
    int scroll;                          /* pixels up from the newest               */
    /* the conversation in the model */
    int phase;                           /* below                                   */
    int sys_len, tail;                   /* tail: 0 clean, 1 after <|im_end|>, 2 mid-reply */
    int sys_web;                         /* the system prompt was made for this web mode */
    int consumed;                        /* reply tokens already turned into text   */
    char question[1024];                 /* UTF-8: the one being answered           */
    uint64_t search_at;
    hit_t hits[3]; int nhits;
    int pending_load;                    /* load this model once the app is open    */
    int want_model;                      /* the one to load at the next open        */
    int prefs_read;
} A;

enum { PH_IDLE, PH_SYSTEM, PH_SEARCH, PH_ANSWER };

/* ---- messages ------------------------------------------------------------------------- */
static void refresh_shown(msg_t *m) {
    /* Only whole UTF-8 sequences: a token can end half-way through one. */
    int n = m->len;
    for (int back = 1; back <= 3 && back <= n; back++) {
        uint8_t c = (uint8_t)m->u8[n - back];
        if ((c & 0xC0) == 0x80) continue;
        if (c >= 0xC0) { int need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2; if (back < need) n -= back; }
        break;
    }
    if (!m->shown) m->shown = kmalloc((uint64_t)m->cap + 1);
    if (!m->shown) return;
    int k = utf8_to_cp1252(m->u8, n, m->shown, m->cap);
    /* the bits of Markdown small models like: **bold**, ### headings, "- " lists */
    int o = 0, bol = 1;
    for (int i = 0; i < k; i++) {
        char c = m->shown[i];
        if (c == '*' && i + 1 < k && m->shown[i + 1] == '*') { i++; continue; }
        if (bol && c == '#') { while (i + 1 < k && (m->shown[i + 1] == '#' || m->shown[i + 1] == ' ')) i++; continue; }
        if (bol && (c == '-' || c == '*') && i + 1 < k && m->shown[i + 1] == ' ') { m->shown[o++] = '\x95'; bol = 0; continue; }
        if (c == '`') continue;
        m->shown[o++] = c;
        bol = c == '\n';
    }
    m->shown[o] = 0;
    m->shown_len = o;
}

static void msg_append(msg_t *m, const char *s, int n) {
    if (m->len + n + 1 > m->cap) {
        int cap = (m->len + n + 1) * 2 < 256 ? 256 : (m->len + n + 1) * 2;
        char *b = kmalloc((uint64_t)cap);
        if (!b) return;
        if (m->u8) { memcpy(b, m->u8, (size_t)m->len); kfree(m->u8); }
        m->u8 = b; m->cap = cap;
        if (m->shown) { kfree(m->shown); m->shown = 0; }
    }
    memcpy(m->u8 + m->len, s, (size_t)n);
    m->len += n; m->u8[m->len] = 0;
    refresh_shown(m);
}

static msg_t *msg_add(int role, const char *u8) {
    if (nmsg == MAX_MSG) {
        if (msgs[0].u8) kfree(msgs[0].u8);
        if (msgs[0].shown) kfree(msgs[0].shown);
        memmove(msgs, msgs + 1, sizeof(msg_t) * (MAX_MSG - 1));
        nmsg--;
    }
    msg_t *m = &msgs[nmsg++];
    memset(m, 0, sizeof *m);
    m->role = role;
    msg_append(m, u8, (int)strlen(u8));
    A.scroll = 0;
    return m;
}

static void msgs_clear(void) {
    for (int i = 0; i < nmsg; i++) { if (msgs[i].u8) kfree(msgs[i].u8); if (msgs[i].shown) kfree(msgs[i].shown); }
    nmsg = 0;
}

/* Latin-1 as typed, to UTF-8 for the model. */
static int to_utf8(const char *in, int n, char *out, int cap) {
    int o = 0;
    for (int i = 0; i < n && o < cap - 3; i++) {
        uint8_t c = (uint8_t)in[i];
        if (c < 0x80) out[o++] = (char)c;
        else { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 63)); }
    }
    out[o] = 0;
    return o;
}

/* ---- preferences: the model last used, the web mode ----------------------------------- */
static void prefs_save(void) {
    if (store_open(1) != 0) return;
    char b[512];
    memset(b, 0, sizeof b);
    snprintf(b, sizeof b, "model=%s\nweb=%d\n", engine_model() >= 0 ? ai_models[engine_model()].id : "", A.web);
    const store_entry_t *e = store_find("localai/prefs");
    if (!e) e = store_create("localai/prefs", sizeof b);
    if (e && !store_write(e, 0, b, sizeof b)) store_progress("localai/prefs", sizeof b, 1, 0);
}

static void prefs_load(void) {
    A.prefs_read = 1;
    models_tidy();
    const store_entry_t *e = store_find("localai/prefs");
    char b[512];
    if (!e || store_read(e, 0, b, sizeof b)) return;
    b[sizeof b - 1] = 0;
    const char *m = strstr(b, "model="), *w = strstr(b, "web=");
    if (w) { A.web = atoi(w + 4); if (A.web < 0 || A.web > 2) A.web = WEB_AUTO; }
    if (m) for (int i = 0; i < ai_nmodels; i++) {
        size_t l = strlen(ai_models[i].id);
        if (!strncmp(m + 6, ai_models[i].id, l) && m[6 + l] == '\n') A.want_model = i;
    }
}

/* ---- prompts ---------------------------------------------------------------------------- */
static int enc(llm_t *m, int *t, int n, int cap, const char *s) { return n + llm_encode(m, s, -1, t + n, cap - n); }
static int tok(int *t, int n, int cap, int id) { if (id >= 0 && n < cap) t[n++] = id; return n; }

static void today(char *out, int cap) {
    static const char *const dn[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const mn[12] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                         "September", "October", "November", "December" };
    out[0] = 0;
    if (!clock_known()) return;
    int y, mo, d, h, mi, s, wd;
    clock_local(&y, &mo, &d, &h, &mi, &s, &wd);
    snprintf(out, (size_t)cap, " Today is %s, %d %s %d.", dn[wd], d, mn[mo - 1], y);
}

static void system_prompt(void) {
    llm_t *m = engine_llm();
    const llm_info_t *I = llm_info(m);
    static int t[512];
    char sys[512], day[64];
    today(day, sizeof day);
    snprintf(sys, sizeof sys, "system\nYou are BoltOS Local AI, a helpful assistant running entirely on a small Orange Pi "
             "computer, with no cloud. Answer clearly and briefly.%s%s", day,
             A.web != WEB_OFF ? " When web search results are given, use them and say what they found." : "");
    int n = 0;
    llm_reset(m);
    n = tok(t, n, 512, I->im_start);
    n = enc(m, t, n, 512, sys);
    n = tok(t, n, 512, I->im_end);
    n = enc(m, t, n, 512, "\n");
    A.sys_len = n;
    A.sys_web = A.web;
    A.tail = 0;
    sampler_t s = { 0, 1, 1, 0, 0, 1 };
    engine_run(t, n, 0, &s);
    A.phase = PH_SYSTEM;
}

/* Does this need the web? */
static int wants_web(const char *q) {
    if (A.web == WEB_OFF) return 0;
    if (A.web == WEB_ALWAYS) return 1;
    static const char *const cues[] = {
        "today", "tonight", "tomorrow", "yesterday", "this week", "this month", "this year", "right now", "currently",
        "current ", "latest", "newest", "recent", "news", "headline", "weather", "forecast", "temperature in",
        "price", "cost of", "stock", "bitcoin", "exchange rate", "score", "who won", "winner", "results",
        "election", "release date", "released", "new version", "update", "2024", "2025", "2026", "2027",
        "search", "look up", "lookup", "google", "on the web", "online", "website", "link to", "open now",
        "near me", "schedule", "when is the next", "how old is", "ceo of", "president of", "prime minister",
    };
    char low[1024];
    int n = 0;
    for (const char *p = q; *p && n < (int)sizeof low - 1; p++) low[n++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    low[n] = 0;
    for (unsigned i = 0; i < sizeof cues / sizeof cues[0]; i++) if (strstr(low, cues[i])) return 1;
    return 0;
}

static void sampler_for(sampler_t *s) {
    const model_t *md = &ai_models[engine_model()];
    s->temp = md->temp; s->top_p = md->top_p; s->top_k = md->top_k;
    s->rep_pen = md->rep_pen; s->rep_last = 64;
    s->seed = now_us() | 1;
}

/* The question (with what the web found) into the model, and an answer out. */
static void ask_model(void) {
    llm_t *m = engine_llm();
    const llm_info_t *I = llm_info(m);
    static int t[4096];
    char *content = kmalloc(8192);
    if (!content) return;
    int cn = 0;
    if (A.nhits > 0) {
        cn += snprintf(content + cn, (size_t)(8192 - cn), "Web search results:\n\n");
        for (int i = 0; i < A.nhits; i++)
            cn += snprintf(content + cn, (size_t)(8192 - cn), "[%d] %s\n%s\n\n", i + 1, A.hits[i].title, A.hits[i].text);
        cn += snprintf(content + cn, (size_t)(8192 - cn), "Use the search results to answer the question. Question: %s", A.question);
    } else snprintf(content, 8192, "%s", A.question);

    for (int attempt = 0; attempt < 2; attempt++) {
        int n = 0;
        if (A.tail == 2) n = tok(t, n, 4096, I->im_end);
        if (A.tail >= 1) n = enc(m, t, n, 4096, "\n");
        n = tok(t, n, 4096, I->im_start);
        n = enc(m, t, n, 4096, "user\n");
        n = enc(m, t, n, 4096, content);
        n = tok(t, n, 4096, I->im_end);
        n = enc(m, t, n, 4096, "\n");
        n = tok(t, n, 4096, I->im_start);
        n = enc(m, t, n, 4096, "assistant\n");
        if (I->arch == ARCH_QWEN3 && I->think >= 0) {             /* non-thinking mode */
            n = tok(t, n, 4096, I->think);
            n = enc(m, t, n, 4096, "\n\n");
            n = tok(t, n, 4096, I->think_end);
            n = enc(m, t, n, 4096, "\n\n");
        }
        int room = I->ctx - llm_pos(m) - n;
        if (room < 300 && attempt == 0 && llm_pos(m) > A.sys_len) {
            llm_rewind(m, A.sys_len);                              /* start over from the system prompt */
            A.tail = 0;
            msg_add(R_NOTE, "The model's memory was full, so it has forgotten the conversation before this.");
            continue;
        }
        if (room < 64) { msg_add(R_NOTE, "That's too long for the model's memory. Try a shorter question, or web search Off."); kfree(content); A.phase = PH_IDLE; return; }
        sampler_t s;
        sampler_for(&s);
        int max_new = room - 8 < 640 ? room - 8 : 640;
        if (engine_run(t, n, max_new, &s) != 0) { msg_add(R_NOTE, "The model is busy."); kfree(content); A.phase = PH_IDLE; return; }
        break;
    }
    kfree(content);
    msg_add(R_AI, "");
    A.consumed = 0;
    A.tail = 2;
    A.phase = PH_ANSWER;
}

static void send_question(void) {
    if (!A.in_len) return;
    if (!engine_llm() || engine_state() == EN_LOADING) { msg_add(R_NOTE, "Choose a model first: press Tab."); return; }
    if (A.phase != PH_IDLE) return;
    to_utf8(A.in, A.in_len, A.question, sizeof A.question);
    msg_add(R_USER, A.question);
    A.in_len = 0; A.in[0] = 0;
    A.nhits = 0;
    if (wants_web(A.question)) { A.phase = PH_SEARCH; A.search_at = now_ms() + 60; }
    else ask_model();
}

static void new_chat(void) {
    if (A.phase == PH_ANSWER) engine_stop();
    msgs_clear();
    if (engine_state() == EN_READY) {
        llm_t *m = engine_llm();
        if (A.sys_web != A.web || !A.sys_len) system_prompt();
        else { llm_rewind(m, A.sys_len); A.tail = 0; }
    }
}

/* ---- every turn of the main loop ---------------------------------------------------------------- */
static int bg(void) {
    int frame = models_poll();
    if (!A.prefs_read && store_is_open()) prefs_load();
    int st0 = engine_state();
    frame |= engine_poll();
    int st = engine_state();
    if (st0 == EN_LOADING && st == EN_READY) {                     /* just loaded */
        prefs_save();
        system_prompt();
        frame = 1;
    }
    if (st0 == EN_LOADING && st == EN_FAILED) { msg_add(R_NOTE, engine_error()); frame = 1; }
    if (A.phase == PH_SYSTEM && engine_done()) { A.phase = PH_IDLE; frame = 1; }
    if (A.phase == PH_SEARCH && now_ms() >= A.search_at) {
        char why[160];
        int n = web_search(A.question, A.hits, 3, why, sizeof why);
        if (n > 0) {
            A.nhits = n;
            char b[1200];
            int bl = snprintf(b, sizeof b, "Searched the web (Exa) for \"%s\":", A.question);
            for (int i = 0; i < n && bl < (int)sizeof b - 300; i++) bl += snprintf(b + bl, sizeof b - (size_t)bl, "\n%d. %s", i + 1, A.hits[i].title);
            msg_add(R_WEB, b);
        } else {
            char b[256];
            snprintf(b, sizeof b, "Web search didn't work (%s), so this answer is from the model alone.", why);
            msg_add(R_NOTE, b);
        }
        if (engine_llm()) ask_model(); else A.phase = PH_IDLE;
        frame = 1;
    }
    if (A.phase == PH_ANSWER && nmsg) {
        llm_t *m = engine_llm();
        const int *out;
        int n = engine_out(&out);
        msg_t *cur = &msgs[nmsg - 1];
        for (; A.consumed < n; A.consumed++) {
            int t = out[A.consumed];
            const llm_info_t *I = llm_info(m);
            if (t == I->im_end || t == I->eos || t == I->eot) { if (t == I->im_end) A.tail = 1; continue; }
            if (llm_is_control(m, t) || t == I->think || t == I->think_end) continue;
            char p[64];
            int pl = llm_piece(m, t, p, sizeof p);
            if (cur->len == 0) while (pl && (p[0] == '\n' || p[0] == ' ')) { memmove(p, p + 1, (size_t)--pl); }
            if (pl) msg_append(cur, p, pl);
            frame = 1;
        }
        cur->ntok = n;
        cur->rate = engine_rate();
        if (engine_done() && A.consumed >= engine_out(0)) {
            A.phase = PH_IDLE;
            if (!cur->len) msg_append(cur, "(stopped)", 9);
            while (cur->len && (cur->u8[cur->len - 1] == '\n' || cur->u8[cur->len - 1] == ' ')) cur->u8[--cur->len] = 0;
            refresh_shown(cur);
            kprintf("[ai] answered: %d tokens at %.1f a second (the question went in at %.1f a second)\n", n,
                    (double)engine_rate(), (double)engine_prompt_rate());
            frame = 1;
        }
    }
    return frame;
}

/* ---- the screen ------------------------------------------------------------------------------- */
static int tab_hit_x, chat_top, chat_bottom;

#define BODY_PX S(14)
#define LINE_H  S(21)

/* Lines of `s` at width w: drawn when draw is set, counted either way. */
static int wrap_lines(const char *s, int n, int x, int y, int w, int face, uint32_t col, int draw, int clip_top, int clip_bot) {
    int lines = 0, i = 0;
    while (i < n) {
        int nl = i;
        while (nl < n && s[nl] != '\n') nl++;
        if (nl == i) { lines++; i++; continue; }                     /* an empty line */
        while (i < nl) {
            int fit = font_fit(face, BODY_PX, s + i, nl - i, w);
            if (fit < nl - i) { int b = fit; while (b > 0 && s[i + b] != ' ') b--; if (b > 0) fit = b; }
            if (fit <= 0) fit = 1;
            int ly = y + lines * LINE_H;
            if (draw && ly + LINE_H > clip_top && ly < clip_bot)
                font_draw(face, BODY_PX, x, ly + font_ascent(face, BODY_PX), s + i, fit, col, 255);
            lines++;
            i += fit;
            while (i < nl && s[i] == ' ') i++;
        }
        i = nl + 1;
    }
    return lines;
}

static const char *short_name(void) {
    int i = engine_model();
    return i >= 0 ? ai_models[i].id : "model";
}

static int msg_height(const msg_t *m, int w) {
    if (!m->shown) return 0;
    int lines = wrap_lines(m->shown, m->shown_len, 0, 0, w, m->role == R_USER ? FONT_MEDIUM : FONT_REGULAR, 0, 0, 0, 0);
    return S(18) + (lines ? lines : 1) * LINE_H + S(16);
}

static void draw_msg(const msg_t *m, int x, int y, int w, int top, int bot) {
    const char *label = m->role == R_USER ? "you" : m->role == R_AI ? short_name() : m->role == R_WEB ? "web" : "note";
    uint32_t lc = m->role == R_AI ? th.accent : th.faint;
    if (y + S(14) > top && y < bot) text_l(x, y + S(7), label, FONT_MONO, S(11), lc);
    if (m->role == R_AI && m->ntok > 1 && m->rate > 0) {
        char b[48];
        snprintf(b, sizeof b, "%d tokens, %.1f/s", m->ntok, (double)m->rate);
        if (y + S(14) > top && y < bot) text_l(x + font_width(FONT_MONO, S(11), label, -1) + S(10), y + S(7), b, FONT_MONO, S(11), th.faint);
    }
    int face = m->role == R_USER ? FONT_MEDIUM : FONT_REGULAR;
    uint32_t col = m->role == R_AI || m->role == R_USER ? th.text : th.sub;
    int lines = wrap_lines(m->shown ? m->shown : "", m->shown_len, x, y + S(18), w, face, col, 1, top, bot);
    if (m->role == R_USER) {
        int h = (lines ? lines : 1) * LINE_H;
        int y0 = y + S(18) > top ? y + S(18) : top, y1 = y + S(18) + h < bot ? y + S(18) + h : bot;
        if (y1 > y0) gfx_fill(x - S(12), y0, S(2), y1 - y0, th.accent);
    }
}

static void draw_status(int x, int y, int w) {
    char b[200];
    int st = engine_state();
    b[0] = 0;
    if (A.phase == PH_SEARCH) snprintf(b, sizeof b, "Searching the web\x85");
    else if (A.phase == PH_SYSTEM) snprintf(b, sizeof b, "Getting ready\x85");
    else if (A.phase == PH_ANSWER) {
        int n = engine_out(0);
        if (!n) {
            int fed = engine_fed(), all = engine_req_len();
            snprintf(b, sizeof b, "Reading the question\x85 %d%%     Esc stops", all ? (int)(fed * 100LL / all) : 0);
        } else snprintf(b, sizeof b, "Writing\x85 %d tokens, %.1f a second     Esc stops", n, (double)engine_rate());
    } else if (st == EN_LOADING) snprintf(b, sizeof b, "Loading %s from the SD card\x85 %d%%", ai_models[engine_model()].name, engine_load_pct());
    if (b[0]) text_l(x, y, b, FONT_REGULAR, S(12), th.sub);
    (void)w;
}

static void draw_chat(int x, int y, int w, int h) {
    int px = x + S(40), pw = w - S(80);
    /* header */
    int hy = y + S(24);
    int tw = text_l(px, hy, "Local AI", FONT_SEMIBOLD, S(17), th.text);
    char b[160];
    int mi = engine_model();
    if (mi >= 0 && engine_state() != EN_FAILED) snprintf(b, sizeof b, "%s  %s", ai_models[mi].name, ai_models[mi].quant);
    else snprintf(b, sizeof b, "no model loaded");
    text_l(px + tw + S(14), hy, b, FONT_MONO, S(11), th.faint);
    snprintf(b, sizeof b, "web search: %s", web_names[A.web]);
    int rx = px + pw;
    rx -= text_r(rx, hy, "Tab  models & settings", FONT_REGULAR, S(11), th.faint) + S(24);
    text_r(rx, hy, b, FONT_MONO, S(11), A.web == WEB_OFF ? th.faint : th.sub);
    tab_hit_x = px + pw - S(300);
    gfx_fill(px, y + S(46), pw, 1, th.line);

    /* the input */
    int ih = S(42), iy = y + h - S(22) - ih;
    gfx_rrect(px, iy, pw, ih, S(4), A.phase == PH_IDLE ? th.accent : th.line, 255);
    gfx_rrect(px + 1, iy + 1, pw - 2, ih - 2, S(3), th.surface, 255);
    int tx = px + S(14), cy = iy + ih / 2, tmax = pw - S(140);
    if (A.in_len) {
        const char *s = A.in;
        while (font_width(FONT_REGULAR, BODY_PX, s, -1) > tmax && *s) s++;    /* the end of a long line */
        int ew = text_l(tx, cy, s, FONT_REGULAR, BODY_PX, th.text);
        gfx_fill(tx + ew + S(1), cy - S(8), S(2), S(16), th.accent);
    } else {
        text_l(tx, cy, engine_llm() ? "Ask anything" : "Press Tab to choose a model", FONT_REGULAR, BODY_PX, th.faint);
        if (A.phase == PH_IDLE && engine_llm()) gfx_fill(tx - S(3), cy - S(8), S(2), S(16), th.accent);
    }
    text_r(px + pw - S(14), cy, A.phase == PH_IDLE ? "Return  send" : "Esc  stop", FONT_REGULAR, S(11), th.faint);
    draw_status(px, iy - S(14), pw);

    /* the conversation, newest at the bottom */
    chat_top = y + S(54); chat_bottom = iy - S(30);
    int cw = pw - S(24), cx = px + S(12);
    gfx_clip_push(x, chat_top, w, chat_bottom - chat_top);
    if (!nmsg) {
        int ey = chat_top + S(40);
        if (engine_state() == EN_LOADING) text_l(cx, ey, "Loading the model into memory. It takes a little while the first time after a start.", FONT_REGULAR, S(14), th.sub);
        else if (!engine_llm()) {
            text_l(cx, ey, "A language model that runs here, on the Pi, with no cloud.", FONT_MEDIUM, S(15), th.text);
            text_l(cx, ey + S(26), "Press Tab to download one from Hugging Face. Questions about today's news, the weather or", FONT_REGULAR, S(14), th.sub);
            text_l(cx, ey + S(48), "prices search the web first, with Exa.", FONT_REGULAR, S(14), th.sub);
            if (engine_state() == EN_FAILED && engine_error()[0]) text_l(cx, ey + S(80), engine_error(), FONT_REGULAR, S(13), 0xE0A030);
        } else text_l(cx, ey, "Ask a question below. Tab changes the model or web search.", FONT_REGULAR, S(14), th.sub);
    }
    int total = 0;
    for (int i = 0; i < nmsg; i++) total += msg_height(&msgs[i], cw);
    int view = chat_bottom - chat_top;
    int max_scroll = total > view ? total - view : 0;
    if (A.scroll > max_scroll) A.scroll = max_scroll;
    int yy = chat_bottom - total + A.scroll;
    if (total < view) yy = chat_top;
    for (int i = 0; i < nmsg; i++) {
        int mh = msg_height(&msgs[i], cw);
        if (yy + mh > chat_top && yy < chat_bottom) draw_msg(&msgs[i], cx, yy, cw, chat_top, chat_bottom);
        yy += mh;
    }
    gfx_clip_pop();
    if (A.scroll) text_r(px + pw, chat_bottom + S(4), "PgDn  newer", FONT_REGULAR, S(11), th.faint);
}

static void human(uint64_t b, char *out, int cap) {
    if (b >= 10000000000ull) snprintf(out, (size_t)cap, "%lu GB", (unsigned long)((b + 500000000) / 1000000000));
    else snprintf(out, (size_t)cap, "%lu MB", (unsigned long)((b + 500000) / 1000000));
}

static int model_rows_y, model_row_h;

static void draw_models(int x, int y, int w, int h) {
    int px = x + S(40), pw = w - S(80);
    int hy = y + S(34);
    text_l(px, hy, "Models", FONT_SEMIBOLD, S(22), th.text);
    text_l(px, hy + S(26), "From Hugging Face, onto the SD card. Only the ones this Pi runs at three or more tokens a second.",
           FONT_REGULAR, S(13), th.sub);
    model_rows_y = hy + S(52); model_row_h = S(66);
    for (int i = 0; i < ai_nmodels; i++) {
        const model_t *md = &ai_models[i];
        int ry = model_rows_y + i * model_row_h, rh = model_row_h - S(6), on = A.sel == i;
        gfx_rrect(px, ry, pw, rh, S(4), on ? th.accent : th.line, 255);
        gfx_rrect(px + 1, ry + 1, pw - 2, rh - 2, S(3), on ? th.sel : th.surface, 255);
        int tw = text_l(px + S(16), ry + S(18), md->name, FONT_SEMIBOLD, S(15), th.text);
        char b[200], sz[24];
        human(md->size, sz, sizeof sz);
        snprintf(b, sizeof b, "%s  %s  %s  about %.0f tokens a second here", md->maker, md->quant, sz, (double)md->tok_s);
        text_l(px + S(16) + tw + S(10), ry + S(18), b, FONT_MONO, S(11), th.faint);
        text_fit_l(px + S(16), ry + S(40), md->about, pw - S(300), FONT_REGULAR, S(13), th.sub);
        uint64_t got, total; uint32_t rate;
        int ms = model_state(i, &got, &total, &rate);
        uint32_t col = th.accent;
        if (engine_model() == i && engine_state() == EN_LOADING) snprintf(b, sizeof b, "Loading %d%%", engine_load_pct());
        else if (engine_model() == i && (engine_state() == EN_READY || engine_state() == EN_BUSY)) { snprintf(b, sizeof b, "In use"); col = th.sub; }
        else if (ms == MS_DOWNLOADING) snprintf(b, sizeof b, "Downloading %d%%  %lu.%lu MB/s", (int)(got * 100 / total),
                                                (unsigned long)(rate / 1000000), (unsigned long)(rate / 100000 % 10));
        else if (ms == MS_CHECKING) snprintf(b, sizeof b, "Checking %d%%", (int)(got * 100 / total));
        else if (ms == MS_READY) snprintf(b, sizeof b, "On the card  " TXT_RARROW " Load");
        else if (ms == MS_PARTIAL) snprintf(b, sizeof b, "%d%% here  " TXT_RARROW " Carry on", (int)(got * 100 / total));
        else snprintf(b, sizeof b, "Download  %s", sz);
        text_r(px + pw - S(16), ry + rh / 2, b, FONT_MEDIUM, S(13), col);
        if (ms == MS_DOWNLOADING || ms == MS_CHECKING) {
            int fill = (int)((uint64_t)(pw - 2) * got / (total ? total : 1));
            gfx_fill(px + 1, ry + rh - S(3), fill, S(2), th.accent);
        }
    }
    int sy = model_rows_y + ai_nmodels * model_row_h + S(8);
    ui_row(px, sy, pw, "Web search, with Exa", web_names[A.web], A.sel == ai_nmodels);
    ui_row(px, sy + S(40), pw, "New chat", nmsg ? "forgets the conversation" : 0, A.sel == ai_nmodels + 1);
    char b[220];
    int by = sy + S(92);
    if (A.confirm) text_l(px, by, "Press Backspace again to delete this model from the SD card.", FONT_MEDIUM, S(13), 0xE0A030);
    else if (model_error()[0]) { snprintf(b, sizeof b, "%s", model_error()); text_l(px, by, b, FONT_MEDIUM, S(13), 0xE5534B); }
    else if (engine_state() == EN_FAILED && engine_error()[0]) text_l(px, by, engine_error(), FONT_MEDIUM, S(13), 0xE0A030);
    else text_l(px, by, "Auto searches when a question is about today, the news, the weather, prices or \"the latest\". "
                "Exa's servers see those questions.", FONT_REGULAR, S(12), th.faint);
    if (store_is_open()) {
        char fr[24];
        human(store_free(), fr, sizeof fr);
        snprintf(b, sizeof b, "SD card: %s free     RAM for models: %d MB + %d MB", fr, (int)(MINI_HIGH_SIZE >> 20), (int)(MINI_LOW_SIZE >> 20));
        text_l(px, y + h - S(24), b, FONT_REGULAR, S(12), th.faint);
    }
    text_r(px + pw, y + h - S(24), "Up/Down  choose     Return  download / load     " TXT_LARROW " " TXT_RARROW "  web search     Backspace  delete     Tab  chat",
           FONT_REGULAR, S(11), th.faint);
}

static void a_draw(int x, int y, int w, int h) {
    if (A.view == V_MODELS) draw_models(x, y, w, h); else draw_chat(x, y, w, h);
}

/* ---- keys and the pointer ------------------------------------------------------------------------- */
static void model_action(int i) {
    int ms = model_state(i, 0, 0, 0);
    if (engine_model() == i && (engine_state() == EN_READY || engine_state() == EN_BUSY)) { A.view = V_CHAT; return; }
    if (ms == MS_READY) {
        if (A.phase != PH_IDLE) { engine_stop(); A.phase = PH_IDLE; }
        msgs_clear();
        engine_load(i);
        A.view = V_CHAT;
        return;
    }
    if (ms == MS_NONE || ms == MS_PARTIAL) model_download(i);
}

static void models_key(const input_ev_t *e) {
    int rows = ai_nmodels + 2;
    switch (e->key) {
    case KEY_UP: if (A.sel > 0) A.sel--; A.confirm = 0; break;
    case KEY_DOWN: if (A.sel < rows - 1) A.sel++; A.confirm = 0; break;
    case KEY_LEFT: case KEY_RIGHT:
        if (A.sel == ai_nmodels) {
            A.web = (A.web + (e->key == KEY_RIGHT ? 1 : 2)) % 3;
            prefs_save();
            if (engine_state() == EN_READY && A.phase == PH_IDLE) new_chat();
        }
        break;
    case KEY_ENTER:
        A.confirm = 0;
        if (A.sel < ai_nmodels) model_action(A.sel);
        else if (A.sel == ai_nmodels) { A.web = (A.web + 1) % 3; prefs_save(); if (engine_state() == EN_READY && A.phase == PH_IDLE) new_chat(); }
        else { new_chat(); A.view = V_CHAT; }
        break;
    case KEY_BACKSPACE: case KEY_DELETE:
        if (A.sel >= ai_nmodels) break;
        if (model_state(A.sel, 0, 0, 0) == MS_NONE) break;
        if (A.confirm) { model_delete(A.sel); A.confirm = 0; }
        else A.confirm = 1;
        break;
    }
}

static void a_key(const input_ev_t *e) {
    if (e->key == KEY_TAB) { A.view = A.view == V_CHAT ? V_MODELS : V_CHAT; A.confirm = 0; return; }
    if (A.view == V_MODELS) { models_key(e); return; }
    switch (e->key) {
    case KEY_ENTER: send_question(); break;
    case KEY_BACKSPACE: if (A.in_len) A.in[--A.in_len] = 0; break;
    case KEY_PGUP: A.scroll += (chat_bottom - chat_top) * 3 / 4; break;
    case KEY_PGDN: A.scroll -= (chat_bottom - chat_top) * 3 / 4; if (A.scroll < 0) A.scroll = 0; break;
    case KEY_UP: A.scroll += LINE_H * 2; break;
    case KEY_DOWN: A.scroll -= LINE_H * 2; if (A.scroll < 0) A.scroll = 0; break;
    case KEY_CHAR:
        if ((e->mods & MOD_CTRL) && (e->ch == 'l' || e->ch == 'L')) { new_chat(); break; }
        if (e->ch >= 32 && A.in_len < (int)sizeof A.in - 1) { A.in[A.in_len++] = (char)e->ch; A.in[A.in_len] = 0; }
        break;
    }
}

static int a_esc(void) {
    if (A.phase == PH_ANSWER) { engine_stop(); return 1; }
    if (A.view == V_MODELS) { A.view = V_CHAT; return 1; }
    if (A.in_len) { A.in_len = 0; A.in[0] = 0; return 1; }
    return 0;
}

static void a_pointer(const input_ev_t *e, int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
    static int was;
    int click = (e->buttons & 1) && !(was & 1);
    was = e->buttons;
    if (e->wheel && A.view == V_CHAT) { A.scroll += e->wheel * LINE_H * 3; if (A.scroll < 0) A.scroll = 0; return; }
    if (!click) return;
    if (A.view == V_CHAT) {
        if (e->y < chat_top && e->x >= tab_hit_x) A.view = V_MODELS;
        return;
    }
    for (int i = 0; i < ai_nmodels; i++)
        if (ui_hit(e->x, e->y, x + S(40), model_rows_y + i * model_row_h, w - S(80), model_row_h - S(6))) {
            if (A.sel == i) model_action(i); else { A.sel = i; A.confirm = 0; }
            return;
        }
}

static void a_open(void) {
    store_open(0);
    if (!A.prefs_read && store_is_open()) prefs_load();
    if (engine_state() == EN_NONE && A.want_model >= 0 && model_state(A.want_model, 0, 0, 0) == MS_READY) {
        engine_load(A.want_model);
        A.want_model = -1;
    }
    /* put away for a video: back now */
    if (engine_state() == EN_FAILED && engine_model() >= 0 && model_state(engine_model(), 0, 0, 0) == MS_READY) {
        msgs_clear();
        engine_load(engine_model());
    }
    if (engine_state() == EN_NONE) {
        int any = 0;
        for (int i = 0; i < ai_nmodels; i++) if (model_state(i, 0, 0, 0) == MS_READY) any = 1;
        if (!any) A.view = V_MODELS;
    }
}

static int a_tick(void) { return 0; }

static const app_t app = { "Local AI", 0x4C8DF6, G_CHAT, a_open, a_key, a_pointer, a_draw, a_tick, a_esc };

/* ---- development: GET /app?localai&<command> ------------------------------------------------------- */
static int dev(const char *cmd, char *out, int cap) {
    int n = 0;
    if (!strncmp(cmd, "ask ", 4)) {
        A.in_len = 0;
        for (const char *p = cmd + 4; *p && A.in_len < (int)sizeof A.in - 1; p++) A.in[A.in_len++] = *p;
        A.in[A.in_len] = 0;
        send_question();
        return snprintf(out, (size_t)cap, "asked\n");
    }
    if (!strncmp(cmd, "load ", 5) || !strncmp(cmd, "download ", 9)) {
        const char *id = strchr(cmd, ' ') + 1;
        for (int i = 0; i < ai_nmodels; i++) if (!strcmp(ai_models[i].id, id)) {
            int r = cmd[0] == 'l' ? engine_load(i) : model_download(i);
            return snprintf(out, (size_t)cap, "%s %s: %d %s\n", cmd[0] == 'l' ? "load" : "download", id, r,
                            r ? (cmd[0] == 'l' ? engine_error() : model_error()) : "");
        }
        return snprintf(out, (size_t)cap, "no model %s\n", id);
    }
    if (!strncmp(cmd, "web ", 4)) { A.web = cmd[4] == 'a' && cmd[5] == 'l' ? WEB_ALWAYS : cmd[4] == 'o' ? WEB_OFF : WEB_AUTO; new_chat(); return snprintf(out, (size_t)cap, "web %s\n", web_names[A.web]); }
    if (!strcmp(cmd, "new")) { new_chat(); return snprintf(out, (size_t)cap, "new chat\n"); }
    if (!strcmp(cmd, "bench")) { int engine_bench(char *out, int cap); return engine_bench(out, cap); }
    if (!strcmp(cmd, "stop")) { engine_stop(); return snprintf(out, (size_t)cap, "stopping\n"); }
    if (!strcmp(cmd, "cancel")) { model_cancel(); return snprintf(out, (size_t)cap, "cancelled\n"); }
    /* status */
    llm_t *m = engine_llm();
    n += snprintf(out + n, (size_t)(cap - n), "engine %d, model %d, phase %d, load %d%%, cores %d, web %s, pos %d (sys %d)\n",
                  engine_state(), engine_model(), A.phase, engine_load_pct(), engine_cores(), web_names[A.web],
                  m ? llm_pos(m) : -1, A.sys_len);
    if (engine_error()[0]) n += snprintf(out + n, (size_t)(cap - n), "engine: %s\n", engine_error());
    if (model_error()[0]) n += snprintf(out + n, (size_t)(cap - n), "models: %s\n", model_error());
    n += snprintf(out + n, (size_t)(cap - n), "last request: prompt %.1f tok/s, reply %.1f tok/s, %d reply tokens, fed %d\n",
                  (double)engine_prompt_rate(), (double)engine_rate(), engine_out(0), engine_fed());
    if (m) {
        llm_stats_t *S = llm_stats(m);
        uint64_t tk = S->tokens ? S->tokens : 1;
        n += snprintf(out + n, (size_t)(cap - n), "per token (%lu): all %.1f ms, products %.1f ms (%lu calls, quantizing %.1f), attention %.1f ms (%.1f on the cores), sampling %.1f ms\n",
                      (unsigned long)S->tokens, S->other_ns / 1e6 / tk, S->matmul_ns / 1e6 / tk, (unsigned long)(S->calls / tk),
                      S->quant_ns / 1e6 / tk, S->attn_ns / 1e6 / tk, S->attn_par_ns / 1e6 / tk, S->sample_ns / 1e6 / tk);
    }
    for (int i = 0; i < ai_nmodels; i++) {
        uint64_t got, total; uint32_t rate;
        int ms = model_state(i, &got, &total, &rate);
        n += snprintf(out + n, (size_t)(cap - n), "  %-14s state %d  %lu/%lu  %u B/s\n", ai_models[i].id, ms,
                      (unsigned long)got, (unsigned long)total, (unsigned)rate);
    }
    for (int i = 0; i < nmsg && n < cap - 200; i++) {
        static const char *const rn[4] = { "you", "ai", "web", "note" };
        n += snprintf(out + n, (size_t)(cap - n), "--- %s%s\n%s\n", rn[msgs[i].role],
                      msgs[i].role == R_AI ? "" : "", msgs[i].u8 ? msgs[i].u8 : "");
        if (n > cap - 1) n = cap - 1;
    }
    return n;
}

static void unload(void) {
    model_cancel();
    engine_unload();
    msgs_clear();
}

static const pkg_info_t info = { PKG_ABI, &app, bg, unload, dev };

const pkg_info_t *pkg_main(void) {
    engine_init();
    models_init();
    A.pending_load = -1; A.want_model = -1;
    kprintf("[ai] BoltOS Local AI is here: %d models on offer\n", ai_nmodels);
    return &info;
}
