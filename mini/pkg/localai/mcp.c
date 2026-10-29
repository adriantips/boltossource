/* ===========================================================================
 *  BoltOS Local AI  -  mcp.c
 *
 *  Web search, from Exa's hosted MCP server (https://mcp.exa.ai/mcp). MCP
 *  is JSON-RPC over HTTPS; the server answers a tools/call without the
 *  initialize handshake, so a search is one POST:
 *
 *      {"method":"tools/call","params":{"name":"web_search_exa",
 *                                       "arguments":{"query":..,"numResults":3}}}
 *
 *  The answer comes as a server-sent event ("data: {...}") whose result is
 *  text: for each page a Title, a URL and Highlights, pages apart by
 *  "---". Those become hits, the highlights cut to a few hundred
 *  characters -- a small model reads every one of them, a token at a time.
 *  No API key: Exa's free tier.
 * ===========================================================================*/
#include "ai.h"
#include "web.h"
#include "kheap.h"
#include "kprintf.h"
#include "string.h"
#include "stdio.h"

#define EXA_MCP  "https://mcp.exa.ai/mcp"
#define MAX_TEXT 200

static int quote(const char *s, char *out, int cap) {
    int n = 0;
    out[n++] = '"';
    for (const unsigned char *p = (const unsigned char *)s; *p && n < cap - 8; p++) {
        if (*p == '"' || *p == '\\') { out[n++] = '\\'; out[n++] = (char)*p; }
        else if (*p == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if (*p < 0x20) out[n++] = ' ';
        else out[n++] = (char)*p;
    }
    out[n++] = '"'; out[n] = 0;
    return n;
}

/* The value after "Name: " on its line. */
static void field(const char *blk, const char *end, const char *name, char *out, int cap) {
    out[0] = 0;
    size_t nl = strlen(name);
    for (const char *p = blk; p && p < end; p = strchr(p, '\n'), p = p ? p + 1 : 0) {
        if (!strncmp(p, name, nl)) {
            p += nl;
            int k = 0;
            while (p < end && *p != '\n' && k < cap - 1) out[k++] = *p++;
            out[k] = 0;
            return;
        }
    }
}

/* The highlights, tidied: markdown furniture and runs of blank lines out,
 * cut at a word near MAX_TEXT. */
static void highlights(const char *blk, const char *end, char *out, int cap) {
    out[0] = 0;
    const char *h = strstr(blk, "Highlights:");
    if (!h || h > end) return;
    h += 11;
    int k = 0, nl = 0;
    for (const char *p = h; p < end && k < cap - 4 && k < MAX_TEXT; p++) {
        char c = *p;
        if (c == '#' || c == '|' || c == '*' || c == '\r') continue;
        if (c == '\n') { if (nl++ || !k) continue; out[k++] = ' '; continue; }
        if (c == ' ' && k && out[k - 1] == ' ') continue;
        nl = 0;
        out[k++] = c;
    }
    if (k >= MAX_TEXT) {
        while (k > MAX_TEXT / 2 && out[k - 1] != ' ') k--;
        out[k++] = '.'; out[k++] = '.'; out[k++] = '.';
    }
    while (k && (out[k - 1] == ' ' || out[k - 1] == '\n')) k--;
    out[k] = 0;
}

int web_search(const char *query, hit_t *hits, int max, char *why, int cap) {
    char q[600], body[900];
    quote(query, q, sizeof q);
    int bl = snprintf(body, sizeof body,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"web_search_exa\","
        "\"arguments\":{\"query\":%s,\"numResults\":%d}}}", q, max);
    int rcap = 96 * 1024;
    char *resp = kmalloc((uint64_t)rcap);
    if (!resp) { snprintf(why, (size_t)cap, "Out of memory"); return -1; }
    int status = 0;
    uint64_t t0 = now_ms();
    int n = web_request("POST", EXA_MCP, "Content-Type: application/json\r\nAccept: application/json, text/event-stream\r\n",
                        body, bl, resp, rcap - 1, &status, 0, 0);
    if (n < 0 || status != 200) {
        snprintf(why, (size_t)cap, n < 0 ? "Exa's server couldn't be reached (%d)" : "Exa's server said %d", n < 0 ? status : status);
        kfree(resp);
        return -1;
    }
    resp[n] = 0;
    /* An event stream: the JSON is on the data: line. */
    char *js = resp;
    char *d = strstr(resp, "data:");
    if (*resp != '{' && d) {
        js = d + 5;
        while (*js == ' ') js++;
        char *e = strchr(js, '\n');
        if (e) *e = 0;
    }
    jarena *ar = 0;
    jv *root = json_parse(js, (int)strlen(js), &ar);
    const char *text = json_s(json_path(root, "result.content.0.text"));
    int got = 0;
    if (!root || !text[0]) {
        const char *msg = json_s(json_path(root, "error.message"));
        snprintf(why, (size_t)cap, "%s", msg[0] ? msg : "Exa's answer didn't make sense");
        json_free(ar); kfree(resp);
        return -1;
    }
    const char *p = text;
    while (*p && got < max) {
        const char *next = strstr(p, "\n---\n");
        const char *end = next ? next : p + strlen(p);
        hit_t *h = &hits[got];
        field(p, end, "Title: ", h->title, sizeof h->title);
        field(p, end, "URL: ", h->url, sizeof h->url);
        highlights(p, end, h->text, sizeof h->text);
        if (!strcmp(h->title, "N/A")) snprintf(h->title, sizeof h->title, "%s", h->url);
        if (h->url[0] || h->text[0]) got++;
        if (!next) break;
        p = next + 5;
    }
    json_free(ar);
    kfree(resp);
    kprintf("[ai] searched the web for \"%s\": %d results in %lu ms\n", query, got, (unsigned long)(now_ms() - t0));
    if (!got) snprintf(why, (size_t)cap, "Nothing came back for that");
    return got;
}
