/* ===========================================================================
 *  BoltOS Local AI  -  ai.h
 *
 *  The package's own pieces, around the engine (llm.h):
 *
 *    engine.c   the model in RAM, run on the Pi's other cores
 *    models.c   the models it offers, fetched from Hugging Face onto the card
 *    mcp.c      web search, through Exa's MCP server
 *    app.c      the chat, the model list, and the package's entry
 * ===========================================================================*/
#pragma once
#include <stdint.h>
#include "mini.h"
#include "ui.h"
#include "pkg.h"
#include "store.h"
#include "llm.h"

/* ---- the models (models.c) ---------------------------------------------------- */
typedef struct {
    const char *id;                       /* "qwen3-0.6b"                          */
    const char *name, *maker, *about;
    const char *repo, *file;              /* on huggingface.co                     */
    const char *quant;
    uint64_t    size;
    const char *sha256;
    int         ctx;                      /* tokens it keeps                       */
    float       tok_s;                    /* measured on an Orange Pi Zero3, 1008 MHz */
    float       temp, top_p; int top_k; float rep_pen;
} model_t;
extern const model_t ai_models[];
extern const int ai_nmodels;

enum { MS_NONE, MS_PARTIAL, MS_DOWNLOADING, MS_CHECKING, MS_READY };
int  model_state(int i, uint64_t *got, uint64_t *total, uint32_t *rate);
int  model_download(int i);               /* starts (or carries on with) it       */
void model_cancel(void);
int  model_delete(int i);
int  model_fits(int i, char *why, int cap);
const char *model_error(void);
int  models_poll(void);                   /* from background(): 1 if anything moved */
void models_init(void);
void models_tidy(void);
void model_path(int i, char *out, int cap);

/* ---- the engine (engine.c) ------------------------------------------------------- */
enum { EN_NONE, EN_LOADING, EN_READY, EN_BUSY, EN_FAILED };
void engine_init(void);
int  engine_state(void);
int  engine_model(void);                  /* which ai_models[] is loaded, -1 none   */
int  engine_load(int i);                  /* from the card, in the background        */
void engine_unload(void);
int  engine_poll(void);
const char *engine_error(void);
int  engine_load_pct(void);
llm_t *engine_llm(void);
int  engine_cores(void);
/* A request: feed these tokens, then (with max_new > 0) write a reply. */
int  engine_run(const int *toks, int n, int max_new, const sampler_t *s);
void engine_stop(void);
int  engine_done(void);                   /* the request finished                   */
int  engine_out(const int **toks);        /* the reply's tokens so far              */
int  engine_fed(void);                    /* how many of the request's went in      */
int  engine_req_len(void);                /* ...of how many                          */
float engine_rate(void);                  /* reply tokens a second, this request    */
float engine_prompt_rate(void);
uint64_t llm_now_ns(void);

/* ---- web search (mcp.c) ------------------------------------------------------------ */
typedef struct { char title[160], url[256], text[700]; } hit_t;
/* Blocks for a second or two (the network keeps turning). The hits' text is
 * UTF-8. Returns how many, or -1 with why. */
int  web_search(const char *query, hit_t *hits, int max, char *why, int cap);
