/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 * Copyright (C) Google Inc.
 * Copyright (C) Alex Zhang
 * Copyright (C) Hanada
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)
#include <zlib.h>
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI)
#include <brotli/encode.h>
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)
#include <zstd.h>
#endif

#if (NGX_EXPR)
#include <ngx_http_expr_module.h>
#endif


#define NGX_HTTP_COMPRESSION_GZIP                 0
#define NGX_HTTP_COMPRESSION_BROTLI               1
#define NGX_HTTP_COMPRESSION_ZSTD                 2
#define NGX_HTTP_COMPRESSION_DEFLATE              3
#define NGX_HTTP_COMPRESSION_IDENTITY             4
#define NGX_HTTP_COMPRESSION_WILDCARD             5
#define NGX_HTTP_COMPRESSION_ENCODINGS            4
#define NGX_HTTP_COMPRESSION_CODINGS              6

#define NGX_HTTP_COMPRESSION_PROCESS              0
#define NGX_HTTP_COMPRESSION_FLUSH                1
#define NGX_HTTP_COMPRESSION_FINISH               2

#define NGX_HTTP_COMPRESSION_MULTI_STATUS         207
#define NGX_HTTP_COMPRESSION_NOT_ACCEPTABLE       406

#define NGX_HTTP_COMPRESSION_ZSTD_CACHE_SIZE      4
#define NGX_HTTP_COMPRESSION_ZSTD_CACHE_MAX       (16 * 1024 * 1024)


typedef struct {
    ngx_uint_t  nelts;
    ngx_uint_t  encodings[NGX_HTTP_COMPRESSION_ENCODINGS];
} ngx_http_compression_encodings_t;


typedef struct {
#if (NGX_EXPR)
    ngx_array_t                           *enable;
    ngx_array_t                           *ignore_qvalue;
    ngx_array_t                           *encodings;
    ngx_array_t                           *min_length;
    ngx_array_t                           *max_length;
#else
    ngx_flag_t                             enable;
    ngx_flag_t                             ignore_qvalue;
    ngx_http_compression_encodings_t      *encodings;
    size_t                                 min_length;
    size_t                                 max_length;
#endif

    ngx_hash_t                             types;
    ngx_array_t                           *types_keys;
    ngx_array_t                           *bypass;
    ngx_bufs_t                             bufs;
    size_t                                 postpone;
    ngx_flag_t                             no_buffer;

#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)
#if (NGX_EXPR)
    ngx_array_t                           *gzip_level;
    ngx_array_t                           *deflate_level;
#else
    ngx_int_t                              gzip_level;
    ngx_int_t                              deflate_level;
#endif

    size_t                                 gzip_window;
    size_t                                 gzip_hash;
    size_t                                 deflate_window;
    size_t                                 deflate_hash;
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI)
#if (NGX_EXPR)
    ngx_array_t                           *brotli_level;
#else
    ngx_int_t                              brotli_level;
#endif

    size_t                                 brotli_window;
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)
#if (NGX_EXPR)
    ngx_array_t                           *zstd_level;
#else
    ngx_int_t                              zstd_level;
#endif

    size_t                                 zstd_window;

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD_TARGET_BLOCK_SIZE)
    size_t                                 zstd_target_block_size;
#endif
#endif
} ngx_http_compression_conf_t;


typedef struct ngx_http_compression_ctx_s  ngx_http_compression_ctx_t;


typedef struct {
    size_t                           size;
    ngx_int_t                      (*init)(ngx_http_compression_ctx_t *ctx);
    ngx_int_t                      (*process)(ngx_http_compression_ctx_t *ctx);
    void                           (*close)(ngx_http_compression_ctx_t *ctx);
} ngx_http_compression_encoder_t;


struct ngx_http_compression_ctx_s {
    ngx_http_request_t                    *request;
    ngx_http_compression_conf_t           *conf;
    const ngx_http_compression_encoder_t  *encoder;

    ngx_chain_t                           *in;
    ngx_chain_t                           *out;
    ngx_chain_t                          **last_out;
    ngx_chain_t                           *free;
    ngx_chain_t                           *busy;
    ngx_buf_t                             *in_buf;
    ngx_buf_t                             *out_buf;

    off_t                                  content_length;
    off_t                                  bytes_in;
    off_t                                  bytes_out;
    size_t                                 postponed;
    ngx_uint_t                             bufs;
    ngx_uint_t                             encoding;
    ngx_uint_t                             operation;
    ngx_int_t                              level;

    unsigned                               buffering:1;
    unsigned                               initialized:1;
    unsigned                               done:1;
    unsigned                               success:1;
};


#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)

typedef struct {
    ngx_http_compression_ctx_t             ctx;
    z_stream                               stream;
    u_char                                *preallocated;
    u_char                                *free_mem;
    u_char                                *end;
    u_char                                 buffer[8];
    u_int                                  pos;
    u_int                                  last;
    unsigned                               flushed:1;
} ngx_http_compression_zlib_ctx_t;

#endif


#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI)

typedef struct {
    ngx_http_compression_ctx_t             ctx;
    BrotliEncoderState                    *state;
} ngx_http_compression_brotli_ctx_t;

#endif


#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)

typedef struct {
    ngx_http_compression_ctx_t             ctx;
    ZSTD_CCtx                             *state;
} ngx_http_compression_zstd_ctx_t;

#endif


static ngx_int_t ngx_http_compression_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_compression_body_filter(ngx_http_request_t *r,
    ngx_chain_t *in);
static ngx_int_t ngx_http_compression_ok(ngx_http_request_t *r);
static ngx_int_t ngx_http_compression_cache_control(ngx_http_request_t *r);
static ngx_int_t ngx_http_compression_select(ngx_http_request_t *r,
    ngx_http_compression_encodings_t *encodings, ngx_flag_t ignore);
static ngx_int_t ngx_http_compression_qvalue(u_char *p, u_char *last);
static ngx_int_t ngx_http_compression_queue(ngx_http_compression_ctx_t *ctx,
    ngx_chain_t *in);
static void ngx_http_compression_release_input(ngx_http_compression_ctx_t *ctx);
static ngx_int_t ngx_http_compression_get_buffer(
    ngx_http_compression_ctx_t *ctx);
static ngx_int_t ngx_http_compression_output(ngx_http_compression_ctx_t *ctx);
static void ngx_http_compression_cleanup(void *data);

#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)
static ngx_int_t ngx_http_compression_zlib_init(
    ngx_http_compression_ctx_t *ctx);
static ngx_int_t ngx_http_compression_zlib_process(
    ngx_http_compression_ctx_t *ctx);
static void ngx_http_compression_zlib_close(ngx_http_compression_ctx_t *ctx);
static void *ngx_http_compression_zlib_alloc(void *opaque, u_int items,
    u_int size);
static void ngx_http_compression_zlib_free(void *opaque, void *address);
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI)
static ngx_int_t ngx_http_compression_brotli_init(
    ngx_http_compression_ctx_t *ctx);
static ngx_int_t ngx_http_compression_brotli_process(
    ngx_http_compression_ctx_t *ctx);
static void ngx_http_compression_brotli_close(ngx_http_compression_ctx_t *ctx);
static void *ngx_http_compression_brotli_alloc(void *opaque, size_t size);
static void ngx_http_compression_brotli_free(void *opaque, void *address);
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)
static ngx_int_t ngx_http_compression_zstd_init(
    ngx_http_compression_ctx_t *ctx);
static ngx_int_t ngx_http_compression_zstd_process(
    ngx_http_compression_ctx_t *ctx);
static void ngx_http_compression_zstd_close(ngx_http_compression_ctx_t *ctx);
static void ngx_http_compression_zstd_exit_process(ngx_cycle_t *cycle);
#endif

static ngx_int_t ngx_http_compression_ratio_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_compression_add_variables(ngx_conf_t *cf);
static char *ngx_http_compression_alias(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_compression_encodings(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_compression_level(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_compression_size(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD_TARGET_BLOCK_SIZE)
static char *ngx_http_compression_zstd_block_size(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
#endif

static void *ngx_http_compression_create_conf(ngx_conf_t *cf);
static char *ngx_http_compression_merge_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_http_compression_filter_init(ngx_conf_t *cf);


#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)

static ngx_conf_num_bounds_t  ngx_http_compression_zlib_bounds = {
    ngx_conf_check_num_bounds, 1, 9
};

#endif

#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI)

static ngx_conf_num_bounds_t  ngx_http_compression_brotli_bounds = {
    ngx_conf_check_num_bounds, 0, 11
};

#endif

static ngx_str_t  ngx_http_compression_names[] = {
    ngx_string("gzip"),
    ngx_string("br"),
    ngx_string("zstd"),
    ngx_string("deflate"),
    ngx_string("identity"),
    ngx_string("*")
};

static ngx_str_t  ngx_http_compression_aliases[] = {
    ngx_string("gzip_http_version"),
    ngx_string("gzip_proxied"),
    ngx_string("gzip_disable")
};

static ngx_http_compression_encodings_t  ngx_http_compression_default = {
#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)
    1, { NGX_HTTP_COMPRESSION_GZIP, 0, 0, 0 }
#elif (NGX_HTTP_COMPRESSION_HAVE_BROTLI)
    1, { NGX_HTTP_COMPRESSION_BROTLI, 0, 0, 0 }
#else
    1, { NGX_HTTP_COMPRESSION_ZSTD, 0, 0, 0 }
#endif
};


static ngx_command_t  ngx_http_compression_commands[] = {

    { ngx_string("compression"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_FLAG,
#if (NGX_EXPR)
      ngx_conf_set_conditional_flag_slot,
#else
      ngx_conf_set_flag_slot,
#endif
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, enable),
      NULL },

    { ngx_string("compression_encodings"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_1MORE,
      ngx_http_compression_encodings,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("compression_ignore_client_qvalue"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_FLAG,
#if (NGX_EXPR)
      ngx_conf_set_conditional_flag_slot,
#else
      ngx_conf_set_flag_slot,
#endif
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, ignore_qvalue),
      NULL },

    { ngx_string("compression_min_length"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_TAKE1,
#if (NGX_EXPR)
      ngx_conf_set_conditional_size_slot,
#else
      ngx_conf_set_size_slot,
#endif
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, min_length),
      NULL },

    { ngx_string("compression_max_length"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_TAKE1,
#if (NGX_EXPR)
      ngx_conf_set_conditional_size_slot,
#else
      ngx_conf_set_size_slot,
#endif
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, max_length),
      NULL },

    { ngx_string("compression_http_version"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_compression_alias,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      &ngx_http_compression_aliases[0] },

    { ngx_string("compression_proxied"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_compression_alias,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      &ngx_http_compression_aliases[1] },

    { ngx_string("compression_disable"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_compression_alias,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      &ngx_http_compression_aliases[2] },

    { ngx_string("compression_types"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_types_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, types_keys),
      &ngx_http_html_default_types[0] },

    { ngx_string("compression_bypass"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
      ngx_http_set_predicate_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, bypass),
      NULL },

    { ngx_string("compression_buffers"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_conf_set_bufs_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, bufs),
      NULL },

    { ngx_string("compression_postpone"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, postpone),
      NULL },

    { ngx_string("compression_no_buffer"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, no_buffer),
      NULL },

#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)

    { ngx_string("compression_gzip_level"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_TAKE1,
      ngx_http_compression_level,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, gzip_level),
      &ngx_http_compression_zlib_bounds },

    { ngx_string("compression_gzip_window"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_compression_size,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, gzip_window),
      NULL },

    { ngx_string("compression_gzip_hash"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_compression_size,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, gzip_hash),
      NULL },

    { ngx_string("compression_deflate_level"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_TAKE1,
      ngx_http_compression_level,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, deflate_level),
      &ngx_http_compression_zlib_bounds },

    { ngx_string("compression_deflate_window"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_compression_size,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, deflate_window),
      NULL },

    { ngx_string("compression_deflate_hash"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_compression_size,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, deflate_hash),
      NULL },

#endif

#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI)

    { ngx_string("compression_brotli_level"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_TAKE1,
      ngx_http_compression_level,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, brotli_level),
      &ngx_http_compression_brotli_bounds },

    { ngx_string("compression_brotli_window"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_compression_size,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, brotli_window),
      NULL },

#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)

    { ngx_string("compression_zstd_level"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_TAKE1,
      ngx_http_compression_level,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, zstd_level),
      NULL },

    { ngx_string("compression_zstd_window"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_compression_size,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, zstd_window),
      NULL },

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD_TARGET_BLOCK_SIZE)

    { ngx_string("compression_zstd_target_block_size"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_compression_zstd_block_size,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_conf_t, zstd_target_block_size),
      NULL },

#endif

#endif

      ngx_null_command
};


static ngx_http_module_t  ngx_http_compression_module_ctx = {
    ngx_http_compression_add_variables,    /* preconfiguration */
    ngx_http_compression_filter_init,      /* postconfiguration */

    NULL,                                  /* create main conf */
    NULL,                                  /* init main conf */

    NULL,                                  /* create server conf */
    NULL,                                  /* merge server conf */

    ngx_http_compression_create_conf,      /* create location conf */
    ngx_http_compression_merge_conf        /* merge location conf */
};


ngx_module_t  ngx_http_compression_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_compression_module_ctx,        /* module context */
    ngx_http_compression_commands,           /* module directives */
    NGX_HTTP_MODULE,                         /* module type */
    NULL,                                    /* init master */
    NULL,                                    /* init module */
    NULL,                                    /* init process */
    NULL,                                    /* init thread */
    NULL,                                    /* exit thread */
#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)
    ngx_http_compression_zstd_exit_process,  /* exit process */
#else
    NULL,                                    /* exit process */
#endif
    NULL,                                    /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_http_variable_t  ngx_http_compression_variables[] = {

    { ngx_string("compression_ratio"), NULL,
      ngx_http_compression_ratio_variable, 0, NGX_HTTP_VAR_NOCACHEABLE, 0 },

      ngx_http_null_variable
};


#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)

static const ngx_http_compression_encoder_t  ngx_http_compression_zlib = {
    sizeof(ngx_http_compression_zlib_ctx_t),
    ngx_http_compression_zlib_init,
    ngx_http_compression_zlib_process,
    ngx_http_compression_zlib_close
};

#endif

#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI)

static const ngx_http_compression_encoder_t  ngx_http_compression_brotli = {
    sizeof(ngx_http_compression_brotli_ctx_t),
    ngx_http_compression_brotli_init,
    ngx_http_compression_brotli_process,
    ngx_http_compression_brotli_close
};

#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)

static const ngx_http_compression_encoder_t  ngx_http_compression_zstd = {
    sizeof(ngx_http_compression_zstd_ctx_t),
    ngx_http_compression_zstd_init,
    ngx_http_compression_zstd_process,
    ngx_http_compression_zstd_close
};

#endif


static const ngx_http_compression_encoder_t
    *ngx_http_compression_encoders[] =
{
#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)
    &ngx_http_compression_zlib,
#else
    NULL,
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI)
    &ngx_http_compression_brotli,
#else
    NULL,
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)
    &ngx_http_compression_zstd,
#else
    NULL,
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)
    &ngx_http_compression_zlib
#else
    NULL
#endif
};


static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;
static u_char                           ngx_http_compression_empty;

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)

static ZSTD_CCtx
    *ngx_http_compression_zstd_cache[NGX_HTTP_COMPRESSION_ZSTD_CACHE_SIZE];

static size_t      ngx_http_compression_zstd_cached_bytes;
static ngx_uint_t  ngx_http_compression_zstd_cached;

#endif


static ngx_int_t
ngx_http_compression_header_filter(ngx_http_request_t *r)
{
    size_t                                 min, max;
    ngx_int_t                              rc, encoding, level;
    ngx_flag_t                             enabled, ignore;
    ngx_table_elt_t                       *h;
    ngx_pool_cleanup_t                    *cln;
    ngx_http_compression_ctx_t            *ctx;
    ngx_http_compression_conf_t           *conf;
    ngx_http_compression_encodings_t      *encodings;
    const ngx_http_compression_encoder_t  *encoder;

    if (r != r->main || r->header_only || r->method == NGX_HTTP_HEAD
        || (r->headers_out.status != NGX_HTTP_OK
            && r->headers_out.status != NGX_HTTP_FORBIDDEN
            && r->headers_out.status != NGX_HTTP_NOT_FOUND
            && r->headers_out.status != NGX_HTTP_COMPRESSION_MULTI_STATUS)
        || (r->headers_out.content_encoding
            && r->headers_out.content_encoding->value.len))
    {
        return ngx_http_next_header_filter(r);
    }

    conf = ngx_http_get_module_loc_conf(r, ngx_http_compression_filter_module);

#if (NGX_EXPR)
    enabled = ngx_http_get_expr_flag_value(r, conf->enable);
#else
    enabled = conf->enable;
#endif

    if (!enabled) {
        return ngx_http_next_header_filter(r);
    }

#if (NGX_EXPR)
    min = ngx_http_get_expr_size_value(r, conf->min_length);
    max = ngx_http_get_expr_size_value(r, conf->max_length);
#else
    min = conf->min_length;
    max = conf->max_length;
#endif

    if ((r->headers_out.content_length_n != -1
         && (r->headers_out.content_length_n < (off_t) min
             || (max && r->headers_out.content_length_n > (off_t) max)))
        || ngx_http_test_content_type(r, &conf->types) == NULL)
    {
        return ngx_http_next_header_filter(r);
    }

    rc = ngx_http_test_predicates(r, conf->bypass);

    if (rc == NGX_ERROR) {
        return NGX_ERROR;
    }

    if (rc == NGX_DECLINED || ngx_http_compression_ok(r) != NGX_OK) {
        return ngx_http_next_header_filter(r);
    }

    r->gzip_vary = 1;

#if (NGX_EXPR)
    encodings = ngx_http_get_expr_ptr_value(r, conf->encodings);
    ignore = ngx_http_get_expr_flag_value(r, conf->ignore_qvalue);
#else
    encodings = conf->encodings;
    ignore = conf->ignore_qvalue;
#endif

    encoding = ngx_http_compression_select(r, encodings, ignore);

    if (encoding == NGX_DECLINED) {
        return ngx_http_next_header_filter(r);
    }

    if (encoding == NGX_HTTP_COMPRESSION_NOT_ACCEPTABLE) {
        return ngx_http_filter_finalize_request(r,
                                           &ngx_http_compression_filter_module,
                                           NGX_HTTP_COMPRESSION_NOT_ACCEPTABLE);
    }

    encoder = ngx_http_compression_encoders[encoding];

    switch (encoding) {

#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)
    case NGX_HTTP_COMPRESSION_GZIP:
#if (NGX_EXPR)
        level = ngx_http_get_expr_num_value(r, conf->gzip_level);
#else
        level = conf->gzip_level;
#endif
        break;
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI)
    case NGX_HTTP_COMPRESSION_BROTLI:
#if (NGX_EXPR)
        level = ngx_http_get_expr_num_value(r, conf->brotli_level);
#else
        level = conf->brotli_level;
#endif
        break;
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)
    case NGX_HTTP_COMPRESSION_ZSTD:
#if (NGX_EXPR)
        level = ngx_http_get_expr_num_value(r, conf->zstd_level);
#else
        level = conf->zstd_level;
#endif
        break;
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)
    case NGX_HTTP_COMPRESSION_DEFLATE:
#if (NGX_EXPR)
        level = ngx_http_get_expr_num_value(r, conf->deflate_level);
#else
        level = conf->deflate_level;
#endif
        break;
#endif

    default:
        return NGX_ERROR;
    }

    ctx = ngx_pcalloc(r->pool, encoder->size);
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        return NGX_ERROR;
    }

    ctx->request = r;
    ctx->conf = conf;
    ctx->encoder = encoder;
    ctx->encoding = encoding;
    ctx->level = level;
    ctx->content_length = r->headers_out.content_length_n;
    ctx->buffering = (conf->postpone != 0);
    ctx->last_out = &ctx->out;

    cln->handler = ngx_http_compression_cleanup;
    cln->data = ctx;

    ngx_http_set_ctx(r, ctx, ngx_http_compression_filter_module);

    h = ngx_list_push(&r->headers_out.headers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    h->hash = 1;
    h->next = NULL;
    ngx_str_set(&h->key, "Content-Encoding");
    h->value = ngx_http_compression_names[encoding];
    r->headers_out.content_encoding = h;

    r->main_filter_need_in_memory = 1;

    ngx_http_clear_content_length(r);
    ngx_http_clear_accept_ranges(r);
    ngx_http_weak_etag(r);

    return ngx_http_next_header_filter(r);
}


static ngx_int_t
ngx_http_compression_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    off_t                        before_in, before_out;
    ngx_int_t                    rc, blocked;
    ngx_chain_t                 *cl;
    ngx_http_compression_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_compression_filter_module);

    if (ctx == NULL || ctx->done || r->header_only) {
        return ngx_http_next_body_filter(r, in);
    }

    if (ngx_http_compression_queue(ctx, in) != NGX_OK) {
        return NGX_ERROR;
    }

    if (in) {
        r->connection->buffered |= NGX_HTTP_GZIP_BUFFERED;
    }

    if (ctx->buffering) {
        return NGX_OK;
    }

    if (!ctx->initialized) {
        if (ctx->encoder->init(ctx) != NGX_OK) {
            return NGX_ERROR;
        }

        ctx->initialized = 1;
    }

    for ( ;; ) {
        blocked = 0;

        for ( ;; ) {
            if (ctx->in_buf == NULL) {
                if (ctx->in == NULL) {
                    break;
                }

                cl = ctx->in;
                ctx->in = cl->next;
                ctx->in_buf = cl->buf;
                ngx_free_chain(r->pool, cl);

                if (ctx->in_buf->last_buf) {
                    ctx->operation = NGX_HTTP_COMPRESSION_FINISH;

                } else if (ctx->in_buf->flush) {
                    ctx->operation = NGX_HTTP_COMPRESSION_FLUSH;

                } else {
                    ctx->operation = NGX_HTTP_COMPRESSION_PROCESS;
                }

                if (ngx_buf_size(ctx->in_buf) == 0
                    && ctx->operation == NGX_HTTP_COMPRESSION_PROCESS)
                {
                    ngx_http_compression_release_input(ctx);
                    continue;
                }
            }

            rc = ngx_http_compression_get_buffer(ctx);

            if (rc == NGX_ERROR) {
                return NGX_ERROR;
            }

            if (rc == NGX_AGAIN) {
                blocked = 1;
                break;
            }

            before_in = ctx->bytes_in;
            before_out = ctx->bytes_out;

            rc = ctx->encoder->process(ctx);

            if (rc == NGX_ERROR) {
                return NGX_ERROR;
            }

            if (rc == NGX_OK
                && ctx->operation != NGX_HTTP_COMPRESSION_PROCESS)
            {
                if (ctx->operation == NGX_HTTP_COMPRESSION_FINISH) {
                    ctx->out_buf->last_buf = 1;
                    ctx->done = 1;
                    ctx->success = 1;

                    ngx_http_compression_cleanup(ctx);
                    r->connection->buffered &= ~NGX_HTTP_GZIP_BUFFERED;

                } else {
                    ctx->out_buf->flush = 1;

                    if (ctx->in == NULL) {
                        r->connection->buffered &= ~NGX_HTTP_GZIP_BUFFERED;
                    }
                }

                if (ngx_http_compression_output(ctx) != NGX_OK) {
                    return NGX_ERROR;
                }

            } else if (ctx->out_buf->last == ctx->out_buf->end) {
                if (ngx_http_compression_output(ctx) != NGX_OK) {
                    return NGX_ERROR;
                }
            }

            if (rc == NGX_OK) {
                ngx_http_compression_release_input(ctx);

            } else if (before_in == ctx->bytes_in
                       && before_out == ctx->bytes_out)
            {
                ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                              "compression encoder made no progress");
                return NGX_ERROR;
            }

            if (ctx->done) {
                break;
            }
        }

        if (ctx->conf->no_buffer && ctx->in_buf == NULL && ctx->in == NULL
            && ctx->out_buf && ctx->out_buf->last != ctx->out_buf->pos)
        {
            if (ngx_http_compression_output(ctx) != NGX_OK) {
                return NGX_ERROR;
            }
        }

        if (ctx->out == NULL && ctx->busy == NULL) {
            return NGX_OK;
        }

        rc = ngx_http_next_body_filter(r, ctx->out);

        if (rc == NGX_ERROR) {
            return NGX_ERROR;
        }

        ngx_chain_update_chains(r->pool, &ctx->free, &ctx->busy, &ctx->out,
                           (ngx_buf_tag_t) &ngx_http_compression_filter_module);
        ctx->last_out = &ctx->out;

        if (ctx->done) {
            return rc;
        }

        if (!blocked) {
            return rc;
        }

        if (ctx->free == NULL) {
            return NGX_AGAIN;
        }
    }
}


static ngx_int_t
ngx_http_compression_ok(ngx_http_request_t *r)
{
    time_t                     date, expires;
    ngx_uint_t                 p;
    ngx_table_elt_t           *e, *d, *cc;
    ngx_http_core_loc_conf_t  *clcf;

    static ngx_str_t  no_cache = ngx_string("no-cache");
    static ngx_str_t  no_store = ngx_string("no-store");
    static ngx_str_t  private = ngx_string("private");

    if (ngx_http_compression_cache_control(r) != NGX_OK) {
        return NGX_DECLINED;
    }

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

    if (r->http_version < clcf->gzip_http_version
        || (r->headers_in.msie6 && clcf->gzip_disable_msie6))
    {
        return NGX_DECLINED;
    }

#if (NGX_HTTP_DEGRADATION)
    if (clcf->gzip_disable_degradation && ngx_http_degraded(r)) {
        return NGX_DECLINED;
    }

#endif

    if (r->headers_in.via == NULL) {
        goto ok;
    }

    p = clcf->gzip_proxied;

    if (p & NGX_HTTP_GZIP_PROXIED_OFF) {
        return NGX_DECLINED;
    }

    if (p & NGX_HTTP_GZIP_PROXIED_ANY) {
        goto ok;
    }

    if (r->headers_in.authorization && (p & NGX_HTTP_GZIP_PROXIED_AUTH)) {
        goto ok;
    }

    e = r->headers_out.expires;

    if (e) {
        if (!(p & NGX_HTTP_GZIP_PROXIED_EXPIRED)) {
            return NGX_DECLINED;
        }

        expires = ngx_parse_http_time(e->value.data, e->value.len);
        if (expires == NGX_ERROR) {
            return NGX_DECLINED;
        }

        d = r->headers_out.date;

        if (d) {
            date = ngx_parse_http_time(d->value.data, d->value.len);
            if (date == NGX_ERROR) {
                return NGX_DECLINED;
            }

        } else {
            date = ngx_time();
        }

        if (expires < date) {
            goto ok;
        }

        return NGX_DECLINED;
    }

    cc = r->headers_out.cache_control;

    if (cc) {
        if ((p & NGX_HTTP_GZIP_PROXIED_NO_CACHE)
            && ngx_http_parse_multi_header_lines(r, cc, &no_cache, NULL))
        {
            goto ok;
        }

        if ((p & NGX_HTTP_GZIP_PROXIED_NO_STORE)
            && ngx_http_parse_multi_header_lines(r, cc, &no_store, NULL))
        {
            goto ok;
        }

        if ((p & NGX_HTTP_GZIP_PROXIED_PRIVATE)
            && ngx_http_parse_multi_header_lines(r, cc, &private, NULL))
        {
            goto ok;
        }

        return NGX_DECLINED;
    }

    if ((p & NGX_HTTP_GZIP_PROXIED_NO_LM) && r->headers_out.last_modified) {
        return NGX_DECLINED;
    }

    if ((p & NGX_HTTP_GZIP_PROXIED_NO_ETAG) && r->headers_out.etag) {
        return NGX_DECLINED;
    }

ok:

#if (NGX_PCRE)
    if (clcf->gzip_disable && r->headers_in.user_agent
        && ngx_regex_exec_array(clcf->gzip_disable,
                                &r->headers_in.user_agent->value,
                                r->connection->log)
           != NGX_DECLINED)
    {
        return NGX_DECLINED;
    }

#endif

    return NGX_OK;
}


static ngx_int_t
ngx_http_compression_cache_control(ngx_http_request_t *r)
{
    u_char           *p, *last, *start;
    ngx_uint_t        i, quoted;
    ngx_list_part_t  *part;
    ngx_table_elt_t  *h;

    part = &r->headers_out.headers.part;
    h = part->elts;

    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            h = part->elts;
            i = 0;
        }

        if (h[i].hash == 0
            || h[i].key.len != sizeof("Cache-Control") - 1
            || ngx_strncasecmp(h[i].key.data, (u_char *) "Cache-Control",
                               sizeof("Cache-Control") - 1) != 0)
        {
            continue;
        }

        p = h[i].value.data;
        last = p + h[i].value.len;

        while (p < last) {
            while (p < last && (*p == ' ' || *p == '\t' || *p == ',')) {
                p++;
            }

            start = p;

            while (p < last && *p != ' ' && *p != '\t'
                   && *p != ',' && *p != '=')
            {
                p++;
            }

            if (p - start == sizeof("no-transform") - 1
                && ngx_strncasecmp(start, (u_char *) "no-transform",
                                   sizeof("no-transform") - 1) == 0)
            {
                return NGX_DECLINED;
            }

            quoted = 0;

            while (p < last) {
                if (*p == ',' && !quoted) {
                    p++;
                    break;
                }

                if (*p == '"') {
                    quoted = !quoted;

                } else if (*p == '\\' && quoted && p + 1 < last) {
                    p++;
                }

                p++;
            }
        }
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_compression_select(ngx_http_request_t *r,
    ngx_http_compression_encodings_t *encodings, ngx_flag_t ignore)
{
    u_char           *p, *last, *start, *end, *name_end;
    ngx_int_t         best, weight, selected, identity;
    ngx_int_t         quality[NGX_HTTP_COMPRESSION_CODINGS];
    ngx_uint_t        i, code;
    ngx_table_elt_t  *h;

    if (r->headers_in.accept_encoding == NULL) {
        return NGX_DECLINED;
    }

    for (i = 0; i < NGX_HTTP_COMPRESSION_CODINGS; i++) {
        quality[i] = -1;
    }

    for (h = r->headers_in.accept_encoding; h; h = h->next) {
        p = h->value.data;
        last = p + h->value.len;

        while (p < last) {
            start = p;
            while (p < last && *p != ',') {
                p++;
            }

            end = p;
            if (p < last) {
                p++;
            }

            while (start < end && (*start == ' ' || *start == '\t')) {
                start++;
            }

            name_end = start;
            while (name_end < end && *name_end != ';'
                   && *name_end != ' ' && *name_end != '\t')
            {
                name_end++;
            }

            for (i = 0; i < NGX_HTTP_COMPRESSION_CODINGS; i++) {
                if ((size_t) (name_end - start)
                       == ngx_http_compression_names[i].len
                    && ngx_strncasecmp(start,
                                       ngx_http_compression_names[i].data,
                                       name_end - start) == 0)
                {
                    if (quality[i] == -1) {
                        quality[i] = ngx_http_compression_qvalue(name_end,
                                                                 end);
                    }

                    break;
                }
            }
        }
    }

    best = 0;
    selected = NGX_DECLINED;

    for (i = 0; i < encodings->nelts; i++) {
        code = encodings->encodings[i];
        weight = quality[code];

        if (weight == -1) {
            weight = quality[NGX_HTTP_COMPRESSION_WILDCARD];
        }

        if (weight > best) {
            best = weight;
            selected = code;

            if (ignore) {
                break;
            }
        }
    }

    identity = quality[NGX_HTTP_COMPRESSION_IDENTITY];

    if (!ignore && identity > best) {
        return NGX_DECLINED;
    }

    if (selected != NGX_DECLINED) {
        return selected;
    }

    if (identity == 0
        || (identity == -1
            && quality[NGX_HTTP_COMPRESSION_WILDCARD] == 0))
    {
        return NGX_HTTP_COMPRESSION_NOT_ACCEPTABLE;
    }

    return NGX_DECLINED;
}


static ngx_int_t
ngx_http_compression_qvalue(u_char *p, u_char *last)
{
    ngx_uint_t  q, n, scale;

    while (p < last && (*p == ' ' || *p == '\t')) {
        p++;
    }

    if (p == last) {
        return 1000;
    }

    if (*p++ != ';') {
        return 0;
    }

    while (p < last && (*p == ' ' || *p == '\t')) {
        p++;
    }

    if (last - p < 3 || ngx_tolower(p[0]) != 'q' || p[1] != '=') {
        return 0;
    }

    p += 2;

    if (*p != '0' && *p != '1') {
        return 0;
    }

    q = (*p++ - '0') * 1000;

    if (p < last && *p == '.') {
        p++;
        n = 0;
        scale = 100;

        while (p < last && *p >= '0' && *p <= '9') {
            if (++n > 3) {
                return 0;
            }

            q += (*p++ - '0') * scale;
            scale /= 10;
        }
    }

    while (p < last && (*p == ' ' || *p == '\t')) {
        p++;
    }

    return (p == last && q <= 1000) ? (ngx_int_t) q : 0;
}


static ngx_int_t
ngx_http_compression_queue(ngx_http_compression_ctx_t *ctx, ngx_chain_t *in)
{
    size_t        size;
    ngx_buf_t    *b;
    ngx_chain_t  *cl, **last;
    ngx_pool_t   *pool;

    pool = ctx->request->pool;
    last = &ctx->in;

    while (*last) {
        last = &(*last)->next;
    }

    for ( /* void */ ; in; in = in->next) {
        b = in->buf;

        if (ngx_buf_size(b) && !ngx_buf_in_memory(b)) {
            ngx_log_error(NGX_LOG_ALERT, ctx->request->connection->log, 0,
                          "compression filter received a file buffer");
            return NGX_ERROR;
        }

        size = ngx_buf_size(b);

        if (ctx->buffering) {
            if (b->flush || b->last_buf
                || size >= ctx->conf->postpone - ctx->postponed)
            {
                ctx->buffering = 0;

            } else {
                ctx->postponed += size;

                if (size) {
                    b = ngx_create_temp_buf(pool, size);
                    if (b == NULL) {
                        return NGX_ERROR;
                    }

                    b->last = ngx_cpymem(b->pos, in->buf->pos, size);
                    b->tag =
                        (ngx_buf_tag_t) &ngx_http_compression_filter_module;
                    in->buf->pos = in->buf->last;
                }
            }
        }

        cl = ngx_alloc_chain_link(pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        cl->buf = b;
        cl->next = NULL;
        *last = cl;
        last = &cl->next;
    }

    return NGX_OK;
}


static void
ngx_http_compression_release_input(ngx_http_compression_ctx_t *ctx)
{
    if (ctx->in_buf->tag
        == (ngx_buf_tag_t) &ngx_http_compression_filter_module
        && ctx->in_buf->start != NULL)
    {
        ngx_pfree(ctx->request->pool, ctx->in_buf->start);
    }

    ctx->in_buf = NULL;
}


static ngx_int_t
ngx_http_compression_get_buffer(ngx_http_compression_ctx_t *ctx)
{
    ngx_buf_t    *b;
    ngx_chain_t  *cl;

    if (ctx->out_buf) {
        return NGX_OK;
    }

    if (ctx->free) {
        cl = ctx->free;
        ctx->free = cl->next;
        b = cl->buf;
        ngx_free_chain(ctx->request->pool, cl);

    } else if (ctx->bufs < (ngx_uint_t) ctx->conf->bufs.num) {
        b = ngx_create_temp_buf(ctx->request->pool, ctx->conf->bufs.size);
        if (b == NULL) {
            return NGX_ERROR;
        }

        ctx->bufs++;

    } else {
        return NGX_AGAIN;
    }

    b->pos = b->start;
    b->last = b->start;
    b->tag = (ngx_buf_tag_t) &ngx_http_compression_filter_module;
    b->temporary = 1;
    b->recycled = 1;
    b->flush = 0;
    b->sync = 0;
    b->last_buf = 0;
    b->last_in_chain = 0;

    ctx->out_buf = b;

    return NGX_OK;
}


static ngx_int_t
ngx_http_compression_output(ngx_http_compression_ctx_t *ctx)
{
    ngx_chain_t  *cl;

    cl = ngx_alloc_chain_link(ctx->request->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    if (ctx->out_buf->pos == ctx->out_buf->last) {
        ctx->out_buf->temporary = 0;
        ctx->out_buf->sync = 1;
    }

    cl->buf = ctx->out_buf;
    cl->next = NULL;
    *ctx->last_out = cl;
    ctx->last_out = &cl->next;
    ctx->out_buf = NULL;

    return NGX_OK;
}


static void
ngx_http_compression_cleanup(void *data)
{
    ngx_http_compression_ctx_t  *ctx = data;

    ctx->encoder->close(ctx);
    ctx->initialized = 0;
}


#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)

static ngx_int_t
ngx_http_compression_zlib_init(ngx_http_compression_ctx_t *ctx)
{
    int                               rc, window, hash;
    size_t                            size;
    ngx_http_compression_conf_t      *conf;
    ngx_http_compression_zlib_ctx_t  *zctx;

    zctx = (ngx_http_compression_zlib_ctx_t *) ctx;
    conf = ctx->conf;

    if (ctx->encoding == NGX_HTTP_COMPRESSION_GZIP) {
        window = conf->gzip_window;
        hash = conf->gzip_hash;

    } else {
        window = conf->deflate_window;
        hash = conf->deflate_hash;
    }

    if (ctx->content_length >= 0) {
        while (window > 9
               && ctx->content_length < ((1 << (window - 1)) - 262))
        {
            window--;
            if (hash > 1) {
                hash--;
            }
        }
    }

    /* Preallocate zlib state, window and hash buffers in one allocation. */

    size = 8192 + 16 + ((size_t) 1 << (window + 2))
           + ((size_t) 1 << (hash + 9));

    zctx->preallocated = ngx_alloc(size, ctx->request->connection->log);
    if (zctx->preallocated == NULL) {
        return NGX_ERROR;
    }

    zctx->free_mem = zctx->preallocated;
    zctx->end = zctx->preallocated + size;

    if (ctx->encoding == NGX_HTTP_COMPRESSION_GZIP) {
        window += 16;
    }

    zctx->stream.zalloc = ngx_http_compression_zlib_alloc;
    zctx->stream.zfree = ngx_http_compression_zlib_free;
    zctx->stream.opaque = zctx;

    rc = deflateInit2(&zctx->stream, (int) ctx->level, Z_DEFLATED,
                      window, hash, Z_DEFAULT_STRATEGY);
    if (rc != Z_OK) {
        ngx_log_error(NGX_LOG_ALERT, ctx->request->connection->log, 0,
                      "deflateInit2() failed: %d", rc);
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_compression_zlib_process(ngx_http_compression_ctx_t *ctx)
{
    int                               rc, operation;
    u_int                             in, out, consumed, produced;
    ngx_uint_t                        buffered;
    ngx_http_compression_zlib_ctx_t  *zctx;

    zctx = (ngx_http_compression_zlib_ctx_t *) ctx;

    if (zctx->last) {
        goto output;
    }

    in = (u_int) ngx_min(ngx_buf_size(ctx->in_buf), UINT_MAX);
    out = (u_int) ngx_min(ctx->out_buf->end - ctx->out_buf->last, UINT_MAX);

    zctx->stream.next_in = in ? ctx->in_buf->pos
                             : &ngx_http_compression_empty;
    zctx->stream.avail_in = in;
    zctx->stream.next_out = ctx->out_buf->last;
    zctx->stream.avail_out = out;

    operation = Z_NO_FLUSH;

    if ((off_t) in == ngx_buf_size(ctx->in_buf)) {
        if (ctx->operation == NGX_HTTP_COMPRESSION_FLUSH) {
            operation = Z_SYNC_FLUSH;

        } else if (ctx->operation == NGX_HTTP_COMPRESSION_FINISH) {
            operation = Z_FINISH;
        }
    }

    /* Z_SYNC_FLUSH needs more than six output bytes to finish reliably. */

    buffered = (operation == Z_SYNC_FLUSH && out <= 6);

    if (buffered) {
        out = sizeof(zctx->buffer);
        zctx->stream.next_out = zctx->buffer;
        zctx->stream.avail_out = out;
    }

    rc = deflate(&zctx->stream, operation);

    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
        ngx_log_error(NGX_LOG_ALERT, ctx->request->connection->log, 0,
                      "deflate() failed: %d", rc);
        return NGX_ERROR;
    }

    consumed = in - zctx->stream.avail_in;
    produced = out - zctx->stream.avail_out;

    if (consumed) {
        ctx->in_buf->pos += consumed;
    }

    ctx->bytes_in += consumed;

    if (buffered) {
        zctx->last = produced;
        zctx->flushed = (ngx_buf_size(ctx->in_buf) == 0
                        && zctx->stream.avail_out);
        goto output;
    }

    ctx->out_buf->last += produced;
    ctx->bytes_out += produced;

    if (rc == Z_STREAM_END) {
        return NGX_OK;
    }

    if (ctx->operation != NGX_HTTP_COMPRESSION_FINISH
        && ngx_buf_size(ctx->in_buf) == 0 && zctx->stream.avail_out)
    {
        return NGX_OK;
    }

    return NGX_AGAIN;

output:

    produced = ngx_min(ctx->out_buf->end - ctx->out_buf->last,
                       zctx->last - zctx->pos);
    ctx->out_buf->last = ngx_cpymem(ctx->out_buf->last,
                                    zctx->buffer + zctx->pos, produced);
    ctx->bytes_out += produced;
    zctx->pos += produced;

    if (zctx->pos == zctx->last) {
        zctx->pos = 0;
        zctx->last = 0;

        if (zctx->flushed) {
            return NGX_OK;
        }
    }

    return NGX_AGAIN;
}


static void
ngx_http_compression_zlib_close(ngx_http_compression_ctx_t *ctx)
{
    ngx_http_compression_zlib_ctx_t  *zctx;

    zctx = (ngx_http_compression_zlib_ctx_t *) ctx;

    if (zctx->stream.state) {
        deflateEnd(&zctx->stream);
    }

    if (zctx->preallocated) {
        ngx_free(zctx->preallocated);
        zctx->preallocated = NULL;
    }
}


static void *
ngx_http_compression_zlib_alloc(void *opaque, u_int items, u_int size)
{
    ngx_http_compression_zlib_ctx_t  *zctx = opaque;

    void    *p;
    size_t   len;

    if (size && items > NGX_MAX_SIZE_T_VALUE / size) {
        return NULL;
    }

    len = (size_t) items * size;

    if (len > NGX_MAX_SIZE_T_VALUE - NGX_ALIGNMENT) {
        return NULL;
    }

    len = ngx_align(len, NGX_ALIGNMENT);

    if (len <= (size_t) (zctx->end - zctx->free_mem)) {
        p = zctx->free_mem;
        zctx->free_mem += len;
        return p;
    }

    return ngx_alloc(len, zctx->ctx.request->connection->log);
}


static void
ngx_http_compression_zlib_free(void *opaque, void *address)
{
    ngx_http_compression_zlib_ctx_t  *zctx = opaque;

    if ((uintptr_t) address >= (uintptr_t) zctx->preallocated
        && (uintptr_t) address < (uintptr_t) zctx->end)
    {
        return;
    }

    ngx_free(address);
}

#endif


#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI)

static ngx_int_t
ngx_http_compression_brotli_init(ngx_http_compression_ctx_t *ctx)
{
    size_t                              window;
    ngx_http_compression_brotli_ctx_t  *bctx;

    bctx = (ngx_http_compression_brotli_ctx_t *) ctx;
    window = ctx->conf->brotli_window;

    if (ctx->content_length >= 0) {
        window = BROTLI_MIN_WINDOW_BITS;

        while (window < ctx->conf->brotli_window
               && ctx->content_length > ((off_t) 1 << window))
        {
            window++;
        }
    }

    bctx->state =
        BrotliEncoderCreateInstance(ngx_http_compression_brotli_alloc,
                                    ngx_http_compression_brotli_free,
                                    ctx->request->connection->log);
    if (bctx->state == NULL) {
        return NGX_ERROR;
    }

    if (!BrotliEncoderSetParameter(bctx->state, BROTLI_PARAM_QUALITY,
                                   (uint32_t) ctx->level)
        || !BrotliEncoderSetParameter(bctx->state, BROTLI_PARAM_LGWIN,
                                      (uint32_t) window))
    {
        ngx_log_error(NGX_LOG_ALERT, ctx->request->connection->log, 0,
                      "BrotliEncoderSetParameter() failed");
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_compression_brotli_process(ngx_http_compression_ctx_t *ctx)
{
    size_t                              in, out, available_in, available_out;
    u_char                             *next_out;
    const u_char                       *next_in;
    BrotliEncoderOperation              operation;
    ngx_http_compression_brotli_ctx_t  *bctx;

    bctx = (ngx_http_compression_brotli_ctx_t *) ctx;

    in = ngx_buf_size(ctx->in_buf);
    out = ctx->out_buf->end - ctx->out_buf->last;
    available_in = in;
    available_out = out;
    next_in = in ? ctx->in_buf->pos : &ngx_http_compression_empty;
    next_out = ctx->out_buf->last;

    if (ctx->operation == NGX_HTTP_COMPRESSION_FINISH) {
        operation = BROTLI_OPERATION_FINISH;

    } else if (ctx->operation == NGX_HTTP_COMPRESSION_FLUSH) {
        operation = BROTLI_OPERATION_FLUSH;

    } else {
        operation = BROTLI_OPERATION_PROCESS;
    }

    if (!BrotliEncoderCompressStream(bctx->state, operation,
                                     &available_in, &next_in,
                                     &available_out, &next_out, NULL))
    {
        ngx_log_error(NGX_LOG_ALERT, ctx->request->connection->log, 0,
                      "BrotliEncoderCompressStream() failed");
        return NGX_ERROR;
    }

    if (in != available_in) {
        ctx->in_buf->pos += in - available_in;
    }

    ctx->out_buf->last = next_out;
    ctx->bytes_in += in - available_in;
    ctx->bytes_out += out - available_out;

    if (ctx->operation == NGX_HTTP_COMPRESSION_FINISH) {
        return BrotliEncoderIsFinished(bctx->state) ? NGX_OK : NGX_AGAIN;
    }

    return (available_in == 0 && !BrotliEncoderHasMoreOutput(bctx->state))
           ? NGX_OK : NGX_AGAIN;
}


static void
ngx_http_compression_brotli_close(ngx_http_compression_ctx_t *ctx)
{
    ngx_http_compression_brotli_ctx_t  *bctx;

    bctx = (ngx_http_compression_brotli_ctx_t *) ctx;

    if (bctx->state) {
        BrotliEncoderDestroyInstance(bctx->state);
        bctx->state = NULL;
    }
}


static void *
ngx_http_compression_brotli_alloc(void *opaque, size_t size)
{
    return ngx_alloc(size, opaque);
}


static void
ngx_http_compression_brotli_free(void *opaque, void *address)
{
    ngx_free(address);
}

#endif


#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)

static ngx_int_t
ngx_http_compression_zstd_init(ngx_http_compression_ctx_t *ctx)
{
    size_t                            rc;
    ngx_uint_t                        n;
    ngx_http_compression_zstd_ctx_t  *zctx;

    zctx = (ngx_http_compression_zstd_ctx_t *) ctx;

    if (ngx_http_compression_zstd_cached) {
        n = --ngx_http_compression_zstd_cached;
        zctx->state = ngx_http_compression_zstd_cache[n];
        ngx_http_compression_zstd_cached_bytes -= ZSTD_sizeof_CCtx(zctx->state);

        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->request->connection->log, 0,
                       "compression zstd context reused");

    } else {
        zctx->state = ZSTD_createCCtx();
        if (zctx->state == NULL) {
            return NGX_ERROR;
        }
    }

    rc = ZSTD_CCtx_reset(zctx->state, ZSTD_reset_session_and_parameters);
    if (ZSTD_isError(rc)) {
        ngx_log_error(NGX_LOG_ALERT, ctx->request->connection->log, 0,
                      "ZSTD_CCtx_reset() failed: %s", ZSTD_getErrorName(rc));
        return NGX_ERROR;
    }

    rc = ZSTD_CCtx_setParameter(zctx->state, ZSTD_c_compressionLevel,
                                (int) ctx->level);
    if (ZSTD_isError(rc)) {
        ngx_log_error(NGX_LOG_ALERT, ctx->request->connection->log, 0,
                      "ZSTD_CCtx_setParameter(compressionLevel) failed: %s",
                      ZSTD_getErrorName(rc));
        return NGX_ERROR;
    }

    rc = ZSTD_CCtx_setParameter(zctx->state, ZSTD_c_windowLog,
                                (int) ctx->conf->zstd_window);
    if (ZSTD_isError(rc)) {
        ngx_log_error(NGX_LOG_ALERT, ctx->request->connection->log, 0,
                      "ZSTD_CCtx_setParameter(windowLog) failed: %s",
                      ZSTD_getErrorName(rc));
        return NGX_ERROR;
    }

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD_TARGET_BLOCK_SIZE)
    rc = ZSTD_CCtx_setParameter(zctx->state, ZSTD_c_targetCBlockSize,
                                (int) ctx->conf->zstd_target_block_size);
    if (ZSTD_isError(rc)) {
        ngx_log_error(NGX_LOG_ALERT, ctx->request->connection->log, 0,
                      "ZSTD_CCtx_setParameter(targetCBlockSize) failed: %s",
                      ZSTD_getErrorName(rc));
        return NGX_ERROR;
    }

#endif

    return NGX_OK;
}


static ngx_int_t
ngx_http_compression_zstd_process(ngx_http_compression_ctx_t *ctx)
{
    size_t                            rc;
    ZSTD_inBuffer                     in;
    ZSTD_outBuffer                    out;
    ZSTD_EndDirective                 operation;
    ngx_http_compression_zstd_ctx_t  *zctx;

    zctx = (ngx_http_compression_zstd_ctx_t *) ctx;

    in.size = ngx_buf_size(ctx->in_buf);
    in.src = in.size ? ctx->in_buf->pos : &ngx_http_compression_empty;
    in.pos = 0;
    out.dst = ctx->out_buf->last;
    out.size = ctx->out_buf->end - ctx->out_buf->last;
    out.pos = 0;

    if (ctx->operation == NGX_HTTP_COMPRESSION_FINISH) {
        operation = ZSTD_e_end;

    } else if (ctx->operation == NGX_HTTP_COMPRESSION_FLUSH) {
        operation = ZSTD_e_flush;

    } else {
        operation = ZSTD_e_continue;
    }

    rc = ZSTD_compressStream2(zctx->state, &out, &in, operation);
    if (ZSTD_isError(rc)) {
        ngx_log_error(NGX_LOG_ALERT, ctx->request->connection->log, 0,
                      "ZSTD_compressStream2() failed: %s",
                      ZSTD_getErrorName(rc));
        return NGX_ERROR;
    }

    if (in.pos) {
        ctx->in_buf->pos += in.pos;
    }

    ctx->out_buf->last += out.pos;
    ctx->bytes_in += in.pos;
    ctx->bytes_out += out.pos;

    if (ctx->operation == NGX_HTTP_COMPRESSION_PROCESS) {
        return (in.pos == in.size && out.pos < out.size) ? NGX_OK : NGX_AGAIN;
    }

    return (in.pos == in.size && rc == 0) ? NGX_OK : NGX_AGAIN;
}


static void
ngx_http_compression_zstd_close(ngx_http_compression_ctx_t *ctx)
{
    size_t                            size;
    ngx_uint_t                        n;
    ngx_http_compression_zstd_ctx_t  *zctx;

    zctx = (ngx_http_compression_zstd_ctx_t *) ctx;

    if (zctx->state == NULL) {
        return;
    }

    size = ZSTD_sizeof_CCtx(zctx->state);

    if (ctx->success
        && ngx_http_compression_zstd_cached
           < NGX_HTTP_COMPRESSION_ZSTD_CACHE_SIZE
        && size <= NGX_HTTP_COMPRESSION_ZSTD_CACHE_MAX
                   - ngx_http_compression_zstd_cached_bytes)
    {
        n = ngx_http_compression_zstd_cached++;
        ngx_http_compression_zstd_cache[n] = zctx->state;
        ngx_http_compression_zstd_cached_bytes += size;

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request->connection->log, 0,
                       "compression zstd context cached: %uz", size);

    } else {
        ZSTD_freeCCtx(zctx->state);

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, ctx->request->connection->log, 0,
                       "compression zstd context freed: %uz", size);
    }

    zctx->state = NULL;
}


static void
ngx_http_compression_zstd_exit_process(ngx_cycle_t *cycle)
{
    ngx_uint_t  n;

    while (ngx_http_compression_zstd_cached) {
        n = --ngx_http_compression_zstd_cached;
        ZSTD_freeCCtx(ngx_http_compression_zstd_cache[n]);
    }

    ngx_http_compression_zstd_cached_bytes = 0;
}

#endif


static ngx_int_t
ngx_http_compression_ratio_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_compression_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_compression_filter_module);

    if (ctx == NULL || !ctx->success || ctx->bytes_out == 0) {
        v->not_found = 1;
        v->no_cacheable = 1;
        return NGX_OK;
    }

    v->data = ngx_pnalloc(r->pool, NGX_OFF_T_LEN + 4);
    if (v->data == NULL) {
        return NGX_ERROR;
    }

    v->len = ngx_sprintf(v->data, "%.2f",
                         (double) ctx->bytes_in / ctx->bytes_out) - v->data;

    v->valid = 1;
    v->no_cacheable = 1;
    v->not_found = 0;

    return NGX_OK;
}


static ngx_int_t
ngx_http_compression_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *v, *var;

    for (v = ngx_http_compression_variables; v->name.len; v++) {
        var = ngx_http_add_variable(cf, &v->name, v->flags);
        if (var == NULL) {
            return NGX_ERROR;
        }

        var->get_handler = v->get_handler;
        var->data = v->data;
    }

    return NGX_OK;
}


static char *
ngx_http_compression_alias(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                 *name;
    ngx_command_t             *core_cmd, local;
    ngx_http_core_loc_conf_t  *clcf;

    name = cmd->post;
    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    for (core_cmd = ngx_http_core_module.commands; core_cmd->name.len;
         core_cmd++)
    {
        if (core_cmd->name.len == name->len
            && ngx_strncmp(core_cmd->name.data, name->data, name->len) == 0)
        {
            local = *core_cmd;
            local.name = cmd->name;

            return core_cmd->set(cf, &local, clcf);
        }
    }

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "could not find core directive \"%V\"", name);
    return NGX_CONF_ERROR;
}


static char *
ngx_http_compression_encodings(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_compression_conf_t  *ccf = conf;

    ngx_uint_t                         i, j, mask;
    ngx_str_t                         *value;
    ngx_http_compression_encodings_t  *encodings;
#if (NGX_EXPR)
    ngx_expr_when_id_t                  id;
    ngx_conf_expr_ptr_ctx_t            *ctx;
#endif

    if (cf->args->nelts > NGX_HTTP_COMPRESSION_ENCODINGS + 1) {
        return "has too many encodings";
    }

#if (NGX_EXPR)
    if (ccf->encodings == NGX_CONF_UNSET_PTR) {
        ccf->encodings = ngx_array_create(cf->pool, 2,
                                          sizeof(ngx_conf_expr_ptr_ctx_t));
        if (ccf->encodings == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    id = ngx_expr_get_associated_when_id(cf);
    ctx = ngx_expr_find_ctx(ccf->encodings, id,
                            sizeof(ngx_conf_expr_ptr_ctx_t),
                            offsetof(ngx_conf_expr_ptr_ctx_t, expr_id));
    if (ctx) {
        return "is duplicate";
    }
#else
    if (ccf->encodings != NGX_CONF_UNSET_PTR) {
        return "is duplicate";
    }

#endif

    encodings = ngx_pcalloc(cf->pool, sizeof(ngx_http_compression_encodings_t));
    if (encodings == NULL) {
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;
    mask = 0;

    for (i = 1; i < cf->args->nelts; i++) {
        for (j = 0; j < NGX_HTTP_COMPRESSION_ENCODINGS; j++) {
            if (value[i].len == ngx_http_compression_names[j].len
                && ngx_strncmp(value[i].data,
                               ngx_http_compression_names[j].data,
                               value[i].len) == 0)
            {
                break;
            }
        }

        if (j == NGX_HTTP_COMPRESSION_ENCODINGS || (mask & (1 << j))) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "invalid or duplicate encoding \"%V\"",
                               &value[i]);
            return NGX_CONF_ERROR;
        }

        if (ngx_http_compression_encoders[j] == NULL) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "encoding \"%V\" is not available", &value[i]);
            return NGX_CONF_ERROR;
        }

        mask |= 1 << j;
        encodings->encodings[encodings->nelts++] = j;
    }

#if (NGX_EXPR)
    ctx = ngx_array_push(ccf->encodings);
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    ctx->expr_id = id;
    ctx->value = encodings;
#else
    ccf->encodings = encodings;
#endif

    return NGX_CONF_OK;
}


static char *
ngx_http_compression_level(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_int_t                 level, sign;
    ngx_str_t                *value;
    ngx_conf_num_bounds_t    *bounds;
#if (NGX_EXPR)
    ngx_array_t             **values;
    ngx_conf_expr_num_ctx_t  *ctx;
    ngx_expr_when_id_t        id;
#else
    ngx_int_t                *np;
#endif

    value = cf->args->elts;
    sign = 1;

    if (value[1].len && value[1].data[0] == '-') {
        sign = -1;
        level = ngx_atoi(value[1].data + 1, value[1].len - 1);

    } else {
        level = ngx_atoi(value[1].data, value[1].len);
    }

    if (level == NGX_ERROR) {
        return "has an invalid compression level";
    }

    level *= sign;
    bounds = cmd->post;

    if (bounds) {
        if (level < bounds->low || level > bounds->high) {
            return "has an invalid compression level";
        }
    }

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)
    if (bounds == NULL
        && (level == 0 || level < ZSTD_minCLevel()
            || level > ZSTD_maxCLevel()))
    {
        return "has an invalid compression level";
    }

#endif

#if (NGX_EXPR)
    values = (ngx_array_t **) ((u_char *) conf + cmd->offset);

    if (*values == NGX_CONF_UNSET_PTR) {
        *values = ngx_array_create(cf->pool, 2,
                                   sizeof(ngx_conf_expr_num_ctx_t));
        if (*values == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    id = ngx_expr_get_associated_when_id(cf);
    ctx = ngx_expr_find_ctx(*values, id, sizeof(ngx_conf_expr_num_ctx_t),
                            offsetof(ngx_conf_expr_num_ctx_t, expr_id));
    if (ctx) {
        return "is duplicate";
    }

    ctx = ngx_array_push(*values);
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    ctx->expr_id = id;
    ctx->value = level;
#else
    np = (ngx_int_t *) ((u_char *) conf + cmd->offset);

    if (*np != NGX_MAX_INT_T_VALUE) {
        return "is duplicate";
    }

    *np = level;
#endif

    return NGX_CONF_OK;
}


static char *
ngx_http_compression_size(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char       *rv;
    size_t     *np, size, n, bits, min, max;
    ngx_str_t  *name;

    rv = ngx_conf_set_size_slot(cf, cmd, conf);
    if (rv != NGX_CONF_OK) {
        return rv;
    }

    np = (size_t *) ((u_char *) conf + cmd->offset);
    size = *np;
    name = &cmd->name;
    min = 512;
    max = 32768;

#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI)
    if (cmd->offset == offsetof(ngx_http_compression_conf_t, brotli_window)) {
        min = (size_t) 1 << BROTLI_MIN_WINDOW_BITS;
        max = (size_t) 1 << BROTLI_MAX_WINDOW_BITS;
    }

#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)
    if (cmd->offset == offsetof(ngx_http_compression_conf_t, gzip_hash)
        || cmd->offset
           == offsetof(ngx_http_compression_conf_t, deflate_hash))
    {
        max = 128 * 1024;
    }

#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)
    if (cmd->offset == offsetof(ngx_http_compression_conf_t, zstd_window)) {
        min = 1024;
        max = 8 * 1024 * 1024;
    }

#endif

    if (size < min || size > max || (size & (size - 1))) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must be a power of two between %uz and %uz",
                           name, min, max);
        return NGX_CONF_ERROR;
    }

    for (n = size, bits = 0; n > 1; n >>= 1) {
        bits++;
    }

    if (max == 128 * 1024) {
        bits -= 8;
    }

    *np = bits;

    return NGX_CONF_OK;
}


#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD_TARGET_BLOCK_SIZE)

static char *
ngx_http_compression_zstd_block_size(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    char         *rv;
    size_t       *size;
    ZSTD_bounds   bounds;

    rv = ngx_conf_set_size_slot(cf, cmd, conf);
    if (rv != NGX_CONF_OK) {
        return rv;
    }

    size = (size_t *) ((u_char *) conf + cmd->offset);
    if (*size == 0) {
        return NGX_CONF_OK;
    }

    bounds = ZSTD_cParam_getBounds(ZSTD_c_targetCBlockSize);

    if (ZSTD_isError(bounds.error)) {
        return "is not supported by this zstd library";
    }

    if (*size < (size_t) bounds.lowerBound
        || *size > (size_t) bounds.upperBound)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "\"%V\" must be between %d and %d",
                           &cmd->name, bounds.lowerBound, bounds.upperBound);
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

#endif


static void *
ngx_http_compression_create_conf(ngx_conf_t *cf)
{
    ngx_http_compression_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_compression_conf_t));
    if (conf == NULL) {
        return NULL;
    }

#if (NGX_EXPR)
    conf->enable = NGX_CONF_UNSET_PTR;
    conf->ignore_qvalue = NGX_CONF_UNSET_PTR;
    conf->min_length = NGX_CONF_UNSET_PTR;
    conf->max_length = NGX_CONF_UNSET_PTR;

#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)
    conf->gzip_level = NGX_CONF_UNSET_PTR;
    conf->deflate_level = NGX_CONF_UNSET_PTR;
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI)
    conf->brotli_level = NGX_CONF_UNSET_PTR;
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)
    conf->zstd_level = NGX_CONF_UNSET_PTR;
#endif

#else
    conf->enable = NGX_CONF_UNSET;
    conf->ignore_qvalue = NGX_CONF_UNSET;
    conf->min_length = NGX_CONF_UNSET_SIZE;
    conf->max_length = NGX_CONF_UNSET_SIZE;

#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)
    conf->gzip_level = NGX_MAX_INT_T_VALUE;
    conf->deflate_level = NGX_MAX_INT_T_VALUE;
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI)
    conf->brotli_level = NGX_MAX_INT_T_VALUE;
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)
    conf->zstd_level = NGX_MAX_INT_T_VALUE;
#endif

#endif

    conf->encodings = NGX_CONF_UNSET_PTR;
    conf->bypass = NGX_CONF_UNSET_PTR;
    conf->no_buffer = NGX_CONF_UNSET;
    conf->postpone = NGX_CONF_UNSET_SIZE;

#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)
    conf->gzip_window = NGX_CONF_UNSET_SIZE;
    conf->gzip_hash = NGX_CONF_UNSET_SIZE;
    conf->deflate_window = NGX_CONF_UNSET_SIZE;
    conf->deflate_hash = NGX_CONF_UNSET_SIZE;
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI)
    conf->brotli_window = NGX_CONF_UNSET_SIZE;
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)
    conf->zstd_window = NGX_CONF_UNSET_SIZE;

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD_TARGET_BLOCK_SIZE)
    conf->zstd_target_block_size = NGX_CONF_UNSET_SIZE;
#endif
#endif

    return conf;
}


static char *
ngx_http_compression_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_compression_conf_t  *prev = parent;
    ngx_http_compression_conf_t  *conf = child;

#if (NGX_EXPR)
    if (ngx_conf_merge_expr_flag_value(cf, &conf->enable,
                                       prev->enable, 0) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    if (ngx_conf_merge_expr_flag_value(cf, &conf->ignore_qvalue,
                                       prev->ignore_qvalue, 0) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    if (ngx_conf_merge_expr_size_value(cf, &conf->min_length,
                                       prev->min_length, 20) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    if (ngx_conf_merge_expr_size_value(cf, &conf->max_length,
                                       prev->max_length, 0) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)
    if (ngx_conf_merge_expr_num_value(cf, &conf->gzip_level,
                                      prev->gzip_level, 1) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    if (ngx_conf_merge_expr_num_value(cf, &conf->deflate_level,
                                      prev->deflate_level, 1) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

#endif

#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI)
    if (ngx_conf_merge_expr_num_value(cf, &conf->brotli_level,
                                      prev->brotli_level, 6) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)
    if (ngx_conf_merge_expr_num_value(cf, &conf->zstd_level,
                                      prev->zstd_level, 1) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

#endif

    if (ngx_conf_merge_expr_ptr_value(cf, &conf->encodings,
                                      prev->encodings,
                                      &ngx_http_compression_default) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }
#else
    ngx_conf_merge_value(conf->enable, prev->enable, 0);
    ngx_conf_merge_value(conf->ignore_qvalue, prev->ignore_qvalue, 0);
    ngx_conf_merge_size_value(conf->min_length, prev->min_length, 20);
    ngx_conf_merge_size_value(conf->max_length, prev->max_length, 0);

#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)
    if (conf->gzip_level == NGX_MAX_INT_T_VALUE) {
        conf->gzip_level = prev->gzip_level == NGX_MAX_INT_T_VALUE
                           ? 1 : prev->gzip_level;
    }

    if (conf->deflate_level == NGX_MAX_INT_T_VALUE) {
        conf->deflate_level = prev->deflate_level == NGX_MAX_INT_T_VALUE
                              ? 1 : prev->deflate_level;
    }

#endif

#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI)
    if (conf->brotli_level == NGX_MAX_INT_T_VALUE) {
        conf->brotli_level = prev->brotli_level == NGX_MAX_INT_T_VALUE
                             ? 6 : prev->brotli_level;
    }

#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)
    if (conf->zstd_level == NGX_MAX_INT_T_VALUE) {
        conf->zstd_level = prev->zstd_level == NGX_MAX_INT_T_VALUE
                           ? 1 : prev->zstd_level;
    }

#endif

    ngx_conf_merge_ptr_value(conf->encodings, prev->encodings,
                             &ngx_http_compression_default);
#endif

    ngx_conf_merge_ptr_value(conf->bypass, prev->bypass, NULL);
    ngx_conf_merge_value(conf->no_buffer, prev->no_buffer, 0);
    ngx_conf_merge_bufs_value(conf->bufs, prev->bufs,
                              (128 * 1024) / ngx_pagesize, ngx_pagesize);
    ngx_conf_merge_size_value(conf->postpone, prev->postpone, 0);

#if (NGX_HTTP_COMPRESSION_HAVE_ZLIB)
    ngx_conf_merge_size_value(conf->gzip_window, prev->gzip_window, 15);
    ngx_conf_merge_size_value(conf->gzip_hash, prev->gzip_hash, 8);
    ngx_conf_merge_size_value(conf->deflate_window, prev->deflate_window, 15);
    ngx_conf_merge_size_value(conf->deflate_hash, prev->deflate_hash, 8);
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI)
    ngx_conf_merge_size_value(conf->brotli_window, prev->brotli_window, 19);
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)
    ngx_conf_merge_size_value(conf->zstd_window, prev->zstd_window, 19);

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD_TARGET_BLOCK_SIZE)
    ngx_conf_merge_size_value(conf->zstd_target_block_size,
                              prev->zstd_target_block_size, 0);
#endif
#endif

    if (ngx_http_merge_types(cf, &conf->types_keys, &conf->types,
                             &prev->types_keys, &prev->types,
                             ngx_http_html_default_types) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_compression_filter_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_compression_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_compression_body_filter;

    return NGX_OK;
}
