/* The browser's parsers, fed bytes nobody would write on purpose.
 *
 * These four files -- kernel/html.c, kernel/dom.c, kernel/layout.c and
 * kernel/js.c -- are the largest untrusted-input surface in the whole system.
 * Everything else on the machine parses something a user typed or a disk we
 * formatted; these parse whatever a web server decided to send, and they do it
 * in ring 0 for the desktop browser. A NUL in the middle of a tag name, a
 * stylesheet that ends mid-declaration, forty thousand nested <div>s: none of
 * that appears on any page anyone tests against, and all of it appears on a
 * page written by someone who wants the machine.
 *
 * The parsers run here exactly as they ship. What is substituted is underneath
 * them -- kmalloc and kfree, which test/host/fuzz.h replaces with an allocator
 * that puts guards either side of every block, poisons what is freed, counts
 * what is leaked, and can refuse to hand out memory partway through a parse.
 * That last one is not decoration: the machine has 64 MB and a page can be
 * bigger than that, and the OOM paths here had never been executed.
 *
 * Build and run through test/t_fuzz.py; --list names the targets. */
#include <stdint.h>

#include "fuzz.h"        /* kmalloc/kfree/the mutator/the runner -- first, it defines the heap */

#include "html.h"
#include "dom.h"
#include "layout.h"
#include "js.h"

/* kprintf: parsers reach for it on their error paths, and a fuzzer that prints
 * a line per input is a fuzzer you cannot watch. Counted, not printed. */
static uint64_t kmsgs;
void kputc(char c) { (void)c; }
void kprintf(const char *fmt, ...) { (void)fmt; kmsgs++; }
void panic(const char *msg) {
    fprintf(stderr, "panic(\"%s\") from a parser -- that is the bug\n", msg);
    abort();
}

/* The input arrives as bytes and most of these want a C string. Copying it
 * into a NUL-terminated buffer is not cheating: html_parse takes an explicit
 * length and is fuzzed with it, but css_color and the selector engine take a
 * `const char *` from a caller that has already terminated it. */
static char strbuf[FZ_MAXLEN + 1];
static const char *as_string(const uint8_t *d, uint32_t n) {
    if (n > FZ_MAXLEN) n = FZ_MAXLEN;
    memcpy(strbuf, d, n);
    strbuf[n] = 0;
    return strbuf;
}

/* -------------------------------------------------------------- the corpus --
 * Small, and each entry is here because it reaches a different part of the
 * code rather than because it looks like a web page. The mutator does the
 * rest; what it cannot do is invent a construct that appears in none of these. */
#define S(x) { (const uint8_t *)(x), (uint32_t)(sizeof(x) - 1) }

static const fz_seed html_corpus[] = {
    S("<!doctype html><html><head><title>Hi</title></head><body><h1>Head</h1>"
      "<p>Some <b>bold</b> and <i>slanted</i> text with a <a href=\"/x\">link</a>.</p>"
      "</body></html>"),
    S("<ul><li>one<li>two<li>three</ul><ol><li>a</li><li>b</li></ol><blockquote>q</blockquote>"),
    S("<style>body{background:#102030;color:rgb(200,200,200)}"
      "h1{font-size:24px;text-align:center}.warn{color:red}#id{background:hsl(120,50%,50%)}</style>"
      "<div class=\"warn\" id=\"id\">styled</div>"),
    S("<table><tr><td>a</td><td>b</td></tr><tr><td colspan=2>c</td></tr></table>"),
    S("<form action=\"/go\"><input name=q value=hi size=20><textarea name=t>body</textarea>"
      "<select name=s><option>1<option>2</select><button>Go</button></form>"),
    S("<pre>  spaced\n  lines\n</pre><code>x&lt;y</code>&amp;&#65;&#x42;&nbsp;&quot;"),
    S("<img src=\"/a.png\" width=64 height=48 alt=pic><br><hr>"),
    S("<p style=\"color:#f00;background:#00f;font-size:150%;text-align:right\">inline</p>"),
    S("<script>var x = 1;</script><link rel=stylesheet href=\"/s.css\"><meta charset=utf-8>"),
    S("<div><div><div><div><span>deep</span></div></div></div></div>"),
    S("<p>unclosed <b>bold <i>italic"),
    S("<!-- a comment --><p>after</p><!-- unterminated"),
    S("<a href=unquoted class=x data-thing='single'>attrs</a>"),
    S("<"), S("<>"), S("<p"), S("&#"), S("<p style="), S(""),
};

static const fz_seed css_corpus[] = {
    S("body { background: #fff; color: #000; margin: 8px; font-size: 16px }"),
    S("h1,h2,h3 { font-weight: bold; text-align: center; padding: 4px 8px }"),
    S(".card > .title { color: rgb(10, 20, 30); border: 1px solid #ccc }"),
    S("#main .row span { display: inline-block; width: 120px; height: 40px }"),
    S("* { margin: 0 } div div div { padding: 1px 2px 3px 4px }"),
    S(".f { display: flex; flex-direction: column; justify-content: space-between;"
      " align-items: center; gap: 12px; flex-grow: 2 }"),
    S(".g { display: grid; grid-template-columns: 1fr 2fr 100px; gap: 4px }"),
    S("a { color: hsla(200, 100%, 40%, 0.5) } b { color: transparent } i { color: rebeccapurple }"),
    S("p{display:none}@media screen{p{display:block}}/* comment */q{color:red}"),
    S("x{"), S("}"), S(":"), S("{;;;}"), S("a{color:}"), S(""),
};

static const fz_seed sel_corpus[] = {
    S("div"), S(".card"), S("#main"), S("*"), S("div p"), S("div > p"),
    S("h1, h2, .x, #y"), S("#main .row > span a"), S("div.card#main"),
    S(""), S(" "), S(">"), S("."), S("#"), S(","), S("a>>b"), S("   >   "),
};

static const fz_seed js_corpus[] = {
    S("var a = 1 + 2 * 3; var s = 'x' + \"y\"; console.log(a, s);"),
    S("function f(n) { if (n < 2) return n; return f(n-1) + f(n-2); } f(10);"),
    S("var o = { a: 1, b: [1,2,3], c: { d: 'e' } }; o.b.push(4); o['a'] += o.b.length;"),
    S("for (var i = 0; i < 5; i++) { if (i % 2) continue; console.log(i); }"),
    S("var i = 0; while (i < 3) { i = i + 1; } do { i--; } while (i > 0);"),
    S("var f = function(x) { return function(y) { return x + y; }; }; f(1)(2);"),
    S("document.getElementById('t').innerHTML = '<b>hi</b>'; document.title = 'set';"),
    S("var e = document.createElement('div'); e.setAttribute('id','n');"
      " document.querySelector('body').appendChild(e);"),
    S("localStorage.setItem('k','v'); var v = localStorage.getItem('k');"
      " document.cookie = 'a=b';"),
    S("setTimeout(function(){ console.log('later'); }, 1);"
      " document.getElementById('t').addEventListener('click', function(){ console.log('c'); });"),
    S("try { null.x; } catch (e) { console.log('caught'); } finally { console.log('f'); }"),
    S("var s = 'abcdef'; s.substring(1,3); s.indexOf('c'); s.split('c'); s.toUpperCase();"),
    S("var a = [3,1,2]; a.sort(); a.join('-'); a.slice(1); a.map(function(x){return x*2;});"),
    S("switch (1) { case 1: console.log('one'); break; default: break; }"),
    S("var x = 1 < 2 ? 'y' : 'n'; x = x || 'z'; x = x && 'w'; x = !x; x = typeof x;"),
    S("1"), S("("), S("{"), S("var"), S("function"), S("''"), S(""),
};

static const fz_seed color_corpus[] = {
    S("#fff"), S("#ff8800"), S("rgb(1,2,3)"), S("rgba(1,2,3,0.5)"),
    S("hsl(360,100%,50%)"), S("hsla(0,0%,0%,1)"), S("transparent"),
    S("rebeccapurple"), S("cornflowerblue"), S("red"), S("#"), S("rgb("),
    S("rgb(999999999999,2,3)"), S("hsl(-1,-1%,-1%)"), S(""),
};

/* ------------------------------------------------------------- the targets --*/

/* The flattener. Both entry points: html_parse_ext takes an external
 * stylesheet as well, and the split point comes out of the input so the
 * mutator gets to move it. */
static void t_html(const uint8_t *d, uint32_t n) {
    uint32_t cut = n ? (uint32_t)(d[0] * 257u) % (n + 1) : 0;
    html_doc *doc = html_parse_ext((const char *)d, n - cut,
                                   (const char *)d + (n - cut), cut);
    if (!doc) return;
    /* Read everything the browser would read, so a run pointing at freed or
     * uninitialised arena memory is touched rather than merely allocated. */
    volatile uint32_t sink = 0;
    for (int i = 0; i < doc->nruns; i++) {
        html_run *r = &doc->runs[i];
        if (r->text) sink += (uint32_t)strlen(r->text);
        if (r->name) sink += (uint32_t)strlen(r->name);
        if (r->elid) sink += (uint32_t)strlen(r->elid);
        if (r->link >= 0 && r->link < doc->nhrefs && doc->hrefs[r->link])
            sink += (uint32_t)strlen(doc->hrefs[r->link]);
        /* Only HRUN_IMG indexes doc->imgs. On an HRUN_INPUT the same field is
         * the control's subtype (html.h says so), and following it as an index
         * reads an imgs[] slot nothing has filled in -- which is how this
         * harness segfaulted before it had fuzzed anything at all. */
        if (r->kind == HRUN_IMG && r->img >= 0 && r->img < doc->nimgs && doc->imgs[r->img])
            sink += (uint32_t)strlen(doc->imgs[r->img]);
        sink += r->style + r->color + r->bg + r->fscale;
    }
    if (doc->title) sink += (uint32_t)strlen(doc->title);
    for (int i = 0; i < doc->nscripts; i++) if (doc->scripts[i]) sink += (uint32_t)strlen(doc->scripts[i]);
    for (int i = 0; i < doc->ncsslinks; i++) if (doc->csslinks[i]) sink += (uint32_t)strlen(doc->csslinks[i]);
    (void)sink;
    html_free(doc);
}

/* Plain text through the same allocator: the browser calls this for anything
 * that is not text/html, so a 4 GB file of newlines goes through it. */
static void t_text(const uint8_t *d, uint32_t n) {
    html_doc *doc = html_parse_text((const char *)d, n);
    if (doc) html_free(doc);
}

/* The tree builder, plus the two serializers, which are where a tree that is
 * legal but strange (an element that is its own ancestor's sibling, an
 * attribute with no name) turns into a buffer write. */
static void t_dom(const uint8_t *d, uint32_t n) {
    dom_document *doc = dom_parse((const char *)d, n);
    if (!doc) return;
    static char out[16384];
    dom_inner_text(doc->root, out, sizeof out);
    dom_inner_html(doc->root, out, sizeof out);
    dom_node *hits[32];
    dom_by_tag(doc->root, "div", hits, 32);
    dom_by_class(doc->root, "x", hits, 32);
    dom_get_by_id(doc, "main");
    dom_free(doc);
}

/* The cascade and the box model, with the input as the stylesheet. */
static const char SAMPLE_DOC[] =
    "<html><body><div id=main class=\"card row\"><h1>T</h1>"
    "<p class=warn>text <b>b</b> <a href=/x>l</a></p>"
    "<div class=f><span>1</span><span>2</span></div>"
    "<div class=g><i>a</i><i>b</i><i>c</i></div></div></body></html>";

static void t_css(const uint8_t *d, uint32_t n) {
    dom_document *doc = dom_parse(SAMPLE_DOC, (uint32_t)(sizeof SAMPLE_DOC - 1));
    if (!doc) return;
    layout_tree *t = layout_build(doc, (const char *)d, n, 800);
    if (t) layout_free(t);
    dom_free(doc);
}

/* The other half: the input is the markup and the stylesheet is fixed, so
 * layout runs over shapes the tree builder produced rather than over one tree. */
static const char SAMPLE_CSS[] =
    "body{margin:8px;color:#222;background:#fff}h1{font-size:24px}"
    ".card{padding:6px;border:1px solid #ccc;width:400px}"
    ".f{display:flex;gap:8px;justify-content:space-between}"
    ".g{display:grid;grid-template-columns:1fr 2fr;gap:4px}"
    "p{display:block;margin:4px 0}span{display:inline-block;width:50px}";

static void t_layout(const uint8_t *d, uint32_t n) {
    dom_document *doc = dom_parse((const char *)d, n);
    if (!doc) return;
    layout_tree *t = layout_build(doc, SAMPLE_CSS, (uint32_t)(sizeof SAMPLE_CSS - 1),
                                  64 + (int)(n % 1200));
    if (t) layout_free(t);
    dom_free(doc);
}

/* The selector engine, against a tree it did not build. dom_specificity has no
 * tree at all and is the one that walks a string with no bounds but its NUL. */
static void t_select(const uint8_t *d, uint32_t n) {
    /* Rebuilt every iteration rather than kept: the guarded heap is wiped
     * between inputs, so a tree held across them would be read after free by
     * the fuzzer itself, and a fuzzer that reports its own bug is worse than
     * no fuzzer. It is a fixed 200-byte parse. */
    dom_document *doc = dom_parse(SAMPLE_DOC, (uint32_t)(sizeof SAMPLE_DOC - 1));
    if (!doc) return;
    const char *sel = as_string(d, n);
    dom_node *hits[32];
    dom_query(doc->root, sel);
    dom_query_all(doc->root, sel, hits, 32);
    dom_specificity(sel);
    if (doc->root->first_child) dom_matches(doc->root->first_child, sel);
    dom_free(doc);
}

/* A colour is a string a stylesheet handed us, and css_color parses six
 * different syntaxes out of it with no length anywhere. */
static void t_color(const uint8_t *d, uint32_t n) {
    css_color(as_string(d, n));
}

/* ---------------------------------------------------------- the JS engine ---
 * The host side is real: a DOM the script can actually reach, so
 * getElementById returns something, innerHTML re-parses, and appendChild links
 * nodes. A host that answers NULL to everything would leave most of the
 * binding code unreached. */
static dom_document *jsdoc;

static js_dom_node h_get_by_id(void *ud, const char *id) { (void)ud; return jsdoc ? dom_get_by_id(jsdoc, id) : NULL; }
static js_dom_node h_get_by_tag(void *ud, const char *tag, int index) {
    (void)ud;
    if (!jsdoc) return NULL;
    dom_node *hits[16];
    int k = dom_by_tag(jsdoc->root, tag, hits, 16);
    return (index >= 0 && index < k) ? hits[index] : NULL;
}
static void h_set_inner(void *ud, js_dom_node nd, const char *html) {
    (void)ud;
    if (jsdoc && nd) dom_set_text(jsdoc, (dom_node *)nd, html);
}
static int h_get_inner(void *ud, js_dom_node nd, char *out, uint32_t cap) {
    (void)ud;
    if (!nd || !cap) return 0;
    return dom_inner_html((dom_node *)nd, out, cap);
}
static void h_set_text(void *ud, js_dom_node nd, const char *text) {
    (void)ud;
    if (jsdoc && nd) dom_set_text(jsdoc, (dom_node *)nd, text);
}
static uint64_t written;
static void h_doc_write(void *ud, const char *html) { (void)ud; written += strlen(html); }
static void h_set_title(void *ud, const char *t) { (void)ud; written += strlen(t); }
static void h_log(void *ud, const char *m) { (void)ud; written += strlen(m); }
static js_dom_node h_query(void *ud, const char *sel) { (void)ud; return jsdoc ? dom_query(jsdoc->root, sel) : NULL; }
static js_dom_node h_create_el(void *ud, const char *tag) { (void)ud; return jsdoc ? dom_create_element(jsdoc, tag) : NULL; }
static void h_append(void *ud, js_dom_node p, js_dom_node c) {
    (void)ud;
    if (p && c) dom_append_child((dom_node *)p, (dom_node *)c);
}
static void h_set_attr(void *ud, js_dom_node nd, const char *k, const char *v) {
    (void)ud;
    if (jsdoc && nd) dom_attr_set(jsdoc, (dom_node *)nd, k, v);
}
static int h_get_attr(void *ud, js_dom_node nd, const char *k, char *out, uint32_t cap) {
    (void)ud;
    if (!nd || !cap) return 0;
    const char *v = dom_attr_get((dom_node *)nd, k);
    if (!v) { out[0] = 0; return 0; }
    uint32_t l = (uint32_t)strlen(v);
    if (l >= cap) l = cap - 1;
    memcpy(out, v, l); out[l] = 0;
    return (int)l;
}
/* One key, so a script that stores and loads gets its value back. */
static char ls_key[64], ls_val[256];
static int h_ls_get(void *ud, const char *k, char *out, uint32_t cap) {
    (void)ud;
    if (!cap || strcmp(k, ls_key)) return -1;
    uint32_t l = (uint32_t)strlen(ls_val);
    if (l >= cap) l = cap - 1;
    memcpy(out, ls_val, l); out[l] = 0;
    return (int)l;
}
static void h_ls_set(void *ud, const char *k, const char *v) {
    (void)ud;
    snprintf(ls_key, sizeof ls_key, "%s", k);
    snprintf(ls_val, sizeof ls_val, "%s", v);
}
static void h_ls_remove(void *ud, const char *k) { (void)ud; if (!strcmp(k, ls_key)) ls_key[0] = 0; }
static int h_cookie_get(void *ud, char *out, uint32_t cap) {
    (void)ud;
    if (!cap) return 0;
    snprintf(out, cap, "a=b; c=d");
    return (int)strlen(out);
}
static void h_cookie_set(void *ud, const char *kv) { (void)ud; written += strlen(kv); }
/* No network in a fuzzer: a fetch that reached one would make every run
 * different. The body is fixed and the status varies, which is what a script
 * branches on. */
static int h_fetch(void *ud, const char *url, char *out, uint32_t cap, int *status) {
    (void)ud;
    if (status) *status = strlen(url) & 1 ? 200 : 404;
    if (!cap) return -1;
    snprintf(out, cap, "{\"ok\":1,\"n\":[1,2,3]}");
    return (int)strlen(out);
}

static js_host jshost = {
    NULL, h_get_by_id, h_get_by_tag, h_set_inner, h_get_inner, h_set_text,
    h_doc_write, h_set_title, h_log, h_query, h_create_el, h_append,
    h_set_attr, h_get_attr, h_ls_get, h_ls_set, h_ls_remove,
    h_cookie_get, h_cookie_set, h_fetch,
};

static void t_js(const uint8_t *d, uint32_t n) {
    char err[256];
    jsdoc = dom_parse(SAMPLE_DOC, (uint32_t)(sizeof SAMPLE_DOC - 1));
    js_run((const char *)d, n, &jshost, err, sizeof err);
    if (jsdoc) { dom_free(jsdoc); jsdoc = NULL; }
}

/* The persistent VM, which is the harder half: the heap outlives the script,
 * timers fire after it returned, and a listener registered by a script that
 * then failed to parse still has to be reachable or freed. */
static void t_jsvm(const uint8_t *d, uint32_t n) {
    char err[256];
    jsdoc = dom_parse(SAMPLE_DOC, (uint32_t)(sizeof SAMPLE_DOC - 1));
    js_vm *vm = js_vm_create(&jshost);
    if (vm) {
        js_vm_eval(vm, (const char *)d, n, err, sizeof err);
        js_vm_pump(vm, 1);
        js_vm_pump(vm, 1000);
        js_vm_has_work(vm);
        if (jsdoc && jsdoc->root->first_child)
            js_vm_dispatch(vm, jsdoc->root->first_child, "click");
        js_vm_destroy(vm);
    }
    if (jsdoc) { dom_free(jsdoc); jsdoc = NULL; }
}

/* Proof that each target reaches the parser it names, before any fuzzing
 * happens. Known input, known answer -- see the note on fz_selftest in
 * test/host/fuzz.h for why this is not optional. */
static char self_log[256];
static void self_log_sink(void *ud, const char *m) { (void)ud; snprintf(self_log, sizeof self_log, "%s", m); }

int fz_selftest(void) {
    fz_check_failures = 0;

    html_doc *d = html_parse("<title>Hi</title><h1>Head</h1><p>text</p>", 40);
    FZ_CHECK(d != NULL);
    if (d) {
        FZ_CHECK(d->nruns >= 2);
        FZ_CHECK(d->title && !strcmp(d->title, "Hi"));
        html_free(d);
    }

    dom_document *doc = dom_parse(SAMPLE_DOC, (uint32_t)(sizeof SAMPLE_DOC - 1));
    FZ_CHECK(doc != NULL);
    if (doc) {
        FZ_CHECK(dom_get_by_id(doc, "main") != NULL);
        FZ_CHECK(dom_query(doc->root, "div > h1") != NULL);
        FZ_CHECK(dom_specificity("#a .b c") == 10101);
        layout_tree *t = layout_build(doc, SAMPLE_CSS, (uint32_t)(sizeof SAMPLE_CSS - 1), 800);
        FZ_CHECK(t != NULL);
        if (t) { FZ_CHECK(t->doc_h > 0); layout_free(t); }
        dom_free(doc);
    }

    FZ_CHECK(css_color("#ff8800") == 0x1FF8800u);
    FZ_CHECK(css_color("rgb(1,2,3)") == 0x1010203u);
    FZ_CHECK(css_color("nonsense") == 0);

    js_host h = jshost; h.log = self_log_sink;
    self_log[0] = 0;
    char err[160] = "";
    int rc = js_run("console.log('' + (1 + 2 * 3));", 29, &h, err, sizeof err);
    FZ_CHECK(rc == 0);
    FZ_CHECK(!strcmp(self_log, "7"));

    /* The recursion guard in kernel/js.c has to stop a runaway script rather
     * than let the process die the way it used to -- and it has to leave room
     * for one that is merely recursive. Twelve frames is more than the
     * fibonacci in the corpus needs, and this is the tighter of the two builds
     * the file is compiled for: the host frames are fatter than the kernel's.
     * If this ever starts failing, JS_STACK_BUDGET is too small or somebody
     * has put a large local back into eval(). */
    rc = js_run("function f(n){ return f(n+1); } f(0);", 36, &h, err, sizeof err);
    FZ_CHECK(rc == -1);
    FZ_CHECK(!strcmp(err, "too much recursion"));

    self_log[0] = 0;
    static const char deep[] =
        "function d(n){ if (n <= 0) return 0; return 1 + d(n-1); }"
        " console.log('' + d(12));";
    rc = js_run(deep, (uint32_t)(sizeof deep - 1), &h, err, sizeof err);
    FZ_CHECK(rc == 0);
    FZ_CHECK(!strcmp(self_log, "12"));

    return fz_check_failures ? -1 : 0;
}

int main(int argc, char **argv) {
    static const fz_target targets[] = {
        { "html",   t_html,   html_corpus,  (int)(sizeof html_corpus  / sizeof *html_corpus),  1 },
        { "text",   t_text,   html_corpus,  (int)(sizeof html_corpus  / sizeof *html_corpus),  1 },
        { "dom",    t_dom,    html_corpus,  (int)(sizeof html_corpus  / sizeof *html_corpus),  1 },
        { "css",    t_css,    css_corpus,   (int)(sizeof css_corpus   / sizeof *css_corpus),   1 },
        { "layout", t_layout, html_corpus,  (int)(sizeof html_corpus  / sizeof *html_corpus),  1 },
        { "select", t_select, sel_corpus,   (int)(sizeof sel_corpus   / sizeof *sel_corpus),   0 },
        { "color",  t_color,  color_corpus, (int)(sizeof color_corpus / sizeof *color_corpus), 0 },
        { "js",     t_js,     js_corpus,    (int)(sizeof js_corpus    / sizeof *js_corpus),    1 },
        { "jsvm",   t_jsvm,   js_corpus,    (int)(sizeof js_corpus    / sizeof *js_corpus),    1 },
    };
    int rc = fz_main(argc, argv, targets, (int)(sizeof targets / sizeof *targets));
    if (kmsgs > (1ull << 40)) fprintf(stderr, "%llu\n", (unsigned long long)kmsgs);  /* never; keeps kmsgs live */
    return rc;
}
