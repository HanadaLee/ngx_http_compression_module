
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 * Copyright (C) Google Inc.
 * Copyright (C) Alex Zhang
 * Copyright (C) Hanada
 */


#include <ngx_http_compression.h>
#include <zlib.h>

#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI_DECODER)
#include <brotli/decode.h>
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD_DECODER)
#include <zstd.h>
#endif


#define NGX_HTTP_DECOMPRESSION_PROCESS    0
#define NGX_HTTP_DECOMPRESSION_FLUSH      1
#define NGX_HTTP_DECOMPRESSION_FINISH     2
#define NGX_HTTP_DECOMPRESSION_CHUNK      (64 * 1024)


typedef struct {
#if (NGX_EXPR)
    ngx_array_t                       *enable;
    ngx_array_t                       *force;
    ngx_array_t                       *encodings;
#else
    ngx_flag_t                         enable;
    ngx_flag_t                         force;
    ngx_http_compression_encodings_t  *encodings;
#endif
    ngx_bufs_t                         bufs;
} ngx_http_decompression_conf_t;


typedef struct ngx_http_decompression_ctx_s  ngx_http_decompression_ctx_t;


typedef struct {
    size_t        size;
    ngx_int_t   (*init)(ngx_http_decompression_ctx_t *ctx);
    ngx_int_t   (*process)(ngx_http_decompression_ctx_t *ctx);
    void        (*close)(ngx_http_decompression_ctx_t *ctx);
} ngx_http_decompression_decoder_t;


struct ngx_http_decompression_ctx_s {
    ngx_http_request_t                      *request;
    ngx_http_decompression_conf_t           *conf;
    const ngx_http_decompression_decoder_t  *decoder;
    ngx_chain_t                             *in;
    ngx_chain_t                             *out;
    ngx_chain_t                            **last_out;
    ngx_chain_t                             *free;
    ngx_chain_t                             *busy;
    ngx_buf_t                               *in_buf;
    ngx_buf_t                               *out_buf;
    off_t                                    bytes_in;
    off_t                                    bytes_out;
    ngx_uint_t                               bufs;
    ngx_uint_t                               encoding;
    ngx_uint_t                               operation;
    unsigned                                 initialized:1;
    unsigned                                 complete:1;
    unsigned                                 done:1;
};


typedef struct {
    ngx_http_decompression_ctx_t  ctx;
    z_stream                      stream;
    u_char                        header[2];
    ngx_uint_t                    header_len;
    ngx_uint_t                    header_pos;
    unsigned                      initialized:1;
} ngx_http_decompression_zlib_ctx_t;


#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI_DECODER)

typedef struct {
    ngx_http_decompression_ctx_t  ctx;
    BrotliDecoderState           *state;
} ngx_http_decompression_brotli_ctx_t;

#endif


#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD_DECODER)

typedef struct {
    ngx_http_decompression_ctx_t  ctx;
    ZSTD_DCtx                    *state;
} ngx_http_decompression_zstd_ctx_t;

#endif


static ngx_int_t ngx_http_decompression_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_decompression_body_filter(ngx_http_request_t *r,
    ngx_chain_t *in);
static ngx_int_t ngx_http_decompression_get_buffer(
    ngx_http_decompression_ctx_t *ctx);
static ngx_int_t ngx_http_decompression_output(
    ngx_http_decompression_ctx_t *ctx);
static ngx_int_t ngx_http_decompression_result(
    ngx_http_decompression_ctx_t *ctx, ngx_flag_t more);
static void ngx_http_decompression_cleanup(void *data);
static ngx_int_t ngx_http_decompression_zlib_init(
    ngx_http_decompression_ctx_t *ctx);
static ngx_int_t ngx_http_decompression_zlib_process(
    ngx_http_decompression_ctx_t *ctx);
static void ngx_http_decompression_zlib_close(
    ngx_http_decompression_ctx_t *ctx);
static void *ngx_http_decompression_zlib_alloc(void *opaque, u_int items,
    u_int  size);
static void ngx_http_decompression_zlib_free(void *opaque, void *address);

#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI_DECODER)
static ngx_int_t ngx_http_decompression_brotli_init(
    ngx_http_decompression_ctx_t *ctx);
static ngx_int_t ngx_http_decompression_brotli_process(
    ngx_http_decompression_ctx_t *ctx);
static void ngx_http_decompression_brotli_close(
    ngx_http_decompression_ctx_t *ctx);
#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD_DECODER)
static ngx_int_t ngx_http_decompression_zstd_init(
    ngx_http_decompression_ctx_t *ctx);
static ngx_int_t ngx_http_decompression_zstd_process(
    ngx_http_decompression_ctx_t *ctx);
static void ngx_http_decompression_zstd_close(
    ngx_http_decompression_ctx_t *ctx);
#endif

static void *ngx_http_decompression_create_conf(ngx_conf_t *cf);
static char *ngx_http_decompression_merge_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_http_decompression_filter_init(ngx_conf_t *cf);


static ngx_command_t  ngx_http_decompression_commands[] = {

    { ngx_string("decompression"),
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
      offsetof(ngx_http_decompression_conf_t, enable),
      NULL },

    { ngx_string("decompression_force"),
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
      offsetof(ngx_http_decompression_conf_t, force),
      NULL },

    { ngx_string("decompression_encodings"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_1MORE,
      ngx_http_compression_encodings,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_decompression_conf_t, encodings),
      &ngx_http_compression_decode_mask },

    { ngx_string("decompression_buffers"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE2,
      ngx_conf_set_bufs_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_decompression_conf_t, bufs),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_decompression_module_ctx = {
    NULL,                                /* preconfiguration */
    ngx_http_decompression_filter_init,  /* postconfiguration */

    NULL,                                /* create main configuration */
    NULL,                                /* init main configuration */

    NULL,                                /* create server configuration */
    NULL,                                /* merge server configuration */

    ngx_http_decompression_create_conf,  /* create location conf */
    ngx_http_decompression_merge_conf    /* merge location conf */
};


ngx_module_t  ngx_http_decompression_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_decompression_module_ctx,  /* module context */
    ngx_http_decompression_commands,     /* module directives */
    NGX_HTTP_MODULE,                     /* module type */
    NULL,                                /* init master */
    NULL,                                /* init module */
    NULL,                                /* init process */
    NULL,                                /* init thread */
    NULL,                                /* exit thread */
    NULL,                                /* exit process */
    NULL,                                /* exit master */
    NGX_MODULE_V1_PADDING
};


static const ngx_http_decompression_decoder_t  ngx_http_decompression_zlib = {
    sizeof(ngx_http_decompression_zlib_ctx_t),
    ngx_http_decompression_zlib_init,
    ngx_http_decompression_zlib_process,
    ngx_http_decompression_zlib_close
};

#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI_DECODER)

static const ngx_http_decompression_decoder_t  ngx_http_decompression_brotli = {
    sizeof(ngx_http_decompression_brotli_ctx_t),
    ngx_http_decompression_brotli_init,
    ngx_http_decompression_brotli_process,
    ngx_http_decompression_brotli_close
};

#endif

#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD_DECODER)

static const ngx_http_decompression_decoder_t  ngx_http_decompression_zstd = {
    sizeof(ngx_http_decompression_zstd_ctx_t),
    ngx_http_decompression_zstd_init,
    ngx_http_decompression_zstd_process,
    ngx_http_decompression_zstd_close
};

#endif

static const ngx_http_decompression_decoder_t
    *ngx_http_decompression_decoders[] =
{
    &ngx_http_decompression_zlib,
#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI_DECODER)
    &ngx_http_decompression_brotli,
#else
    NULL,
#endif
#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD_DECODER)
    &ngx_http_decompression_zstd,
#else
    NULL,
#endif
    &ngx_http_decompression_zlib
};


static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;
static ngx_buf_t                        ngx_http_decompression_empty_buf;


static ngx_int_t
ngx_http_decompression_header_filter(ngx_http_request_t *r)
{
    ngx_uint_t                               i, encoding;
    ngx_flag_t                               enabled, force;
    ngx_str_t                               *value;
    ngx_pool_cleanup_t                      *cln;
    ngx_http_decompression_ctx_t            *ctx;
    ngx_http_decompression_conf_t           *conf;
    ngx_http_compression_encodings_t        *encodings;
    const ngx_http_decompression_decoder_t  *decoder;

    conf = ngx_http_get_module_loc_conf(r,
                                       ngx_http_decompression_filter_module);

#if (NGX_EXPR)
    enabled = ngx_http_get_expr_flag_value(r, conf->enable);
#else
    enabled = conf->enable;
#endif

    if (!enabled || r->headers_out.content_encoding == NULL
        || r->headers_out.content_range != NULL
        || r->headers_out.status == NGX_HTTP_PARTIAL_CONTENT
        || ngx_http_compression_cache_control(r) != NGX_OK)
    {
        return ngx_http_next_header_filter(r);
    }

#if (NGX_EXPR)
    force = ngx_http_get_expr_flag_value(r, conf->force);
    encodings = ngx_http_get_expr_ptr_value(r, conf->encodings);
#else
    force = conf->force;
    encodings = conf->encodings;
#endif

    value = &r->headers_out.content_encoding->value;
    encoding = NGX_HTTP_COMPRESSION_ENCODINGS;

    for (i = 0; i < encodings->nelts; i++) {
        if (value->len
            == ngx_http_compression_names[encodings->encodings[i]].len
            && ngx_strncasecmp(value->data,
                   ngx_http_compression_names[encodings->encodings[i]].data,
                   value->len) == 0)
        {
            encoding = encodings->encodings[i];
            break;
        }
    }

    if (encoding == NGX_HTTP_COMPRESSION_ENCODINGS) {
        return ngx_http_next_header_filter(r);
    }

    if (!force) {
        r->gzip_vary = 1;

        if (ngx_http_compression_accepted(r, encoding) == NGX_OK
            && ngx_http_compression_ok(r) == NGX_OK)
        {
            return ngx_http_next_header_filter(r);
        }
    }

    decoder = ngx_http_decompression_decoders[encoding];
    ctx = ngx_pcalloc(r->pool, decoder->size);
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    cln = ngx_pool_cleanup_add(r->pool, 0);
    if (cln == NULL) {
        return NGX_ERROR;
    }

    ctx->request = r;
    ctx->conf = conf;
    ctx->decoder = decoder;
    ctx->encoding = encoding;
    ctx->last_out = &ctx->out;
    cln->handler = ngx_http_decompression_cleanup;
    cln->data = ctx;

    ngx_http_set_ctx(r, ctx, ngx_http_decompression_filter_module);
    r->filter_need_in_memory = 1;
    r->headers_out.content_encoding->hash = 0;
    r->headers_out.content_encoding = NULL;

    ngx_http_clear_content_length(r);
    ngx_http_clear_accept_ranges(r);
    ngx_http_weak_etag(r);

    return  ngx_http_next_header_filter(r);
}


static ngx_int_t
ngx_http_decompression_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    off_t                          before_in, before_out;
    ngx_int_t                      rc;
    ngx_uint_t                     blocked;
    ngx_chain_t                   *cl;
    ngx_http_decompression_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_decompression_filter_module);

    if (ctx == NULL || ctx->done || r->header_only) {
        return ngx_http_next_body_filter(r, in);
    }

    for (cl = in; cl; cl = cl->next) {
        if (ngx_buf_size(cl->buf) && !ngx_buf_in_memory(cl->buf)) {
            ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                          "decompression filter received a file buffer");
            goto failed;
        }
    }

    if (ngx_chain_add_copy(r->pool, &ctx->in, in) != NGX_OK) {
        goto failed;
    }

    if (!ctx->initialized) {
        if (ctx->decoder->init(ctx) != NGX_OK) {
            goto failed;
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

                if (ctx->in_buf->last_buf
                    || (r != r->main && ctx->in_buf->last_in_chain))
                {
                    ctx->operation = NGX_HTTP_DECOMPRESSION_FINISH;

                } else if (ctx->in_buf->flush) {
                    ctx->operation = NGX_HTTP_DECOMPRESSION_FLUSH;

                } else {
                    ctx->operation = NGX_HTTP_DECOMPRESSION_PROCESS;
                }

                if (ngx_buf_size(ctx->in_buf) == 0
                    && ctx->operation == NGX_HTTP_DECOMPRESSION_PROCESS)
                {
                    ctx->in_buf = NULL;
                    continue;
                }
            }

            rc = ngx_http_decompression_get_buffer(ctx);
            if (rc == NGX_ERROR) {
                goto failed;
            }

            if (rc == NGX_AGAIN) {
                blocked = 1;
                break;
            }

            before_in = ctx->bytes_in;
            before_out = ctx->bytes_out;
            rc = ctx->decoder->process(ctx);

            if (rc == NGX_ERROR) {
                goto failed;
            }

            if (rc == NGX_OK
                && ctx->operation != NGX_HTTP_DECOMPRESSION_PROCESS)
            {
                if (ctx->operation == NGX_HTTP_DECOMPRESSION_FINISH) {
                    ctx->out_buf->last_buf = (r == r->main);
                    ctx->out_buf->last_in_chain = 1;
                    ctx->done = 1;
                    ngx_http_decompression_cleanup(ctx);

                } else {
                    ctx->out_buf->flush = 1;
                }

                if (ngx_http_decompression_output(ctx) != NGX_OK) {
                    goto failed;
                }

            } else if (ctx->out_buf->last == ctx->out_buf->end) {
                if (ngx_http_decompression_output(ctx) != NGX_OK) {
                    goto failed;
                }
            }

            if (rc == NGX_OK) {
                ctx->in_buf = NULL;

            } else if (before_in == ctx->bytes_in
                       && before_out == ctx->bytes_out)
            {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                              "decompression decoder made no progress");
                goto failed;
            }

            if (rc == NGX_AGAIN && ngx_buf_size(ctx->in_buf) == 0) {
                ctx->in_buf = &ngx_http_decompression_empty_buf;
            }

            if (ctx->done) {
                break;
            }
        }

        if (ctx->out_buf && ctx->out_buf->last != ctx->out_buf->pos) {
            if (ngx_http_decompression_output(ctx) != NGX_OK) {
                goto failed;
            }
        }

        rc = NGX_OK;

        if (ctx->out || ctx->busy) {
            rc = ngx_http_next_body_filter(r, ctx->out);
            if (rc == NGX_ERROR) {
                goto failed;
            }

            ngx_chain_update_chains(r->pool, &ctx->free, &ctx->busy, &ctx->out,
                         (ngx_buf_tag_t) &ngx_http_decompression_filter_module);
            ctx->last_out = &ctx->out;
        }

        if (ctx->done || !blocked) {
            return rc;
        }

        if (ctx->free == NULL) {
            return NGX_AGAIN;
        }
    }

failed:

    ctx->done = 1;
    ngx_http_decompression_cleanup(ctx);

    return  NGX_ERROR;
}


static ngx_int_t
ngx_http_decompression_get_buffer(ngx_http_decompression_ctx_t *ctx)
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
    b->tag = (ngx_buf_tag_t) &ngx_http_decompression_filter_module;
    b->temporary = 1;
    b->recycled = 1;
    b->flush = 0;
    b->sync = 0;
    b->last_buf = 0;
    b->last_in_chain = 0;

    ctx->out_buf = b;

    return  NGX_OK;
}


static ngx_int_t
ngx_http_decompression_output(ngx_http_decompression_ctx_t *ctx)
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

    return  NGX_OK;
}


static ngx_int_t
ngx_http_decompression_result(ngx_http_decompression_ctx_t *ctx,
    ngx_flag_t more)
{
    if (ngx_buf_size(ctx->in_buf) || more) {
        return NGX_AGAIN;
    }

    if (ctx->operation == NGX_HTTP_DECOMPRESSION_FINISH && !ctx->complete) {
        ngx_log_error(NGX_LOG_ERR, ctx->request->connection->log, 0,
                      "truncated %V response",
                      &ngx_http_compression_names[ctx->encoding]);
        return NGX_ERROR;
    }

    return  NGX_OK;
}


static void
ngx_http_decompression_cleanup(void *data)
{
    ngx_http_decompression_ctx_t  *ctx = data;

    ctx->decoder->close(ctx);
    ctx->initialized = 0;
}


static ngx_int_t
ngx_http_decompression_zlib_init(ngx_http_decompression_ctx_t *ctx)
{
    ngx_http_decompression_zlib_ctx_t  *zctx;

    zctx = (ngx_http_decompression_zlib_ctx_t *) ctx;
    zctx->stream.zalloc = ngx_http_decompression_zlib_alloc;
    zctx->stream.zfree = ngx_http_decompression_zlib_free;
    zctx->stream.opaque = ctx;

    if (ctx->encoding == NGX_HTTP_COMPRESSION_GZIP) {
        if (inflateInit2(&zctx->stream, MAX_WBITS + 16) != Z_OK) {
            return NGX_ERROR;
        }

        zctx->initialized = 1;
    }

    return  NGX_OK;
}


static ngx_int_t
ngx_http_decompression_zlib_process(ngx_http_decompression_ctx_t *ctx)
{
    int                                 rc, window;
    u_int                               input, output, consumed, produced;
    ngx_flag_t                          prefix;
    ngx_http_decompression_zlib_ctx_t  *zctx;

    zctx = (ngx_http_decompression_zlib_ctx_t *) ctx;

    if (ctx->complete) {
        if (ngx_buf_size(ctx->in_buf) == 0) {
            return NGX_OK;
        }

        if (ctx->encoding != NGX_HTTP_COMPRESSION_GZIP) {
            ngx_log_error(NGX_LOG_ERR, ctx->request->connection->log, 0,
                          "trailing data in deflate response");
            return NGX_ERROR;
        }

        if (inflateReset(&zctx->stream) != Z_OK) {
            return NGX_ERROR;
        }

        ctx->complete = 0;
    }

    if (!zctx->initialized) {
        while (zctx->header_len < 2 && ngx_buf_size(ctx->in_buf)) {
            zctx->header[zctx->header_len++] = *ctx->in_buf->pos++;
            ctx->bytes_in++;
        }

        if (zctx->header_len < 2) {
            return ngx_http_decompression_result(ctx, 0);
        }

        window = -MAX_WBITS;

        if ((zctx->header[0] & 0x0f) == Z_DEFLATED
            && (zctx->header[0] >> 4) <= 7
            && ((zctx->header[0] << 8) + zctx->header[1]) % 31 == 0)
        {
            window = MAX_WBITS;
        }

        if (inflateInit2(&zctx->stream, window) != Z_OK) {
            return NGX_ERROR;
        }

        zctx->initialized = 1;
    }

    prefix = (zctx->header_pos != zctx->header_len);

    if (prefix) {
        input = (u_int) (zctx->header_len - zctx->header_pos);
        zctx->stream.next_in = zctx->header + zctx->header_pos;

    } else {
        input = (u_int) ngx_min(ngx_buf_size(ctx->in_buf),
                               NGX_HTTP_DECOMPRESSION_CHUNK);
        zctx->stream.next_in = input ? ctx->in_buf->pos : Z_NULL;
    }

    output = (u_int) ngx_min(ctx->out_buf->end - ctx->out_buf->last,
                            NGX_HTTP_DECOMPRESSION_CHUNK);
    zctx->stream.avail_in = input;
    zctx->stream.next_out = ctx->out_buf->last;
    zctx->stream.avail_out = output;

    rc = inflate(&zctx->stream, Z_NO_FLUSH);
    consumed = input - zctx->stream.avail_in;
    produced = output - zctx->stream.avail_out;

    if (prefix) {
        zctx->header_pos += consumed;

    } else if (consumed) {
        ctx->in_buf->pos += consumed;
        ctx->bytes_in += consumed;
    }

    ctx->out_buf->last += produced;
    ctx->bytes_out += produced;

    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
        ngx_log_error(NGX_LOG_ERR, ctx->request->connection->log, 0,
                      "inflate() failed: %d", rc);
        return NGX_ERROR;
    }

    ctx->complete = (rc == Z_STREAM_END);

    return ngx_http_decompression_result(ctx,
                zctx->header_pos != zctx->header_len
                || (!ctx->complete && zctx->stream.avail_out == 0));
}


static void
ngx_http_decompression_zlib_close(ngx_http_decompression_ctx_t *ctx)
{
    ngx_http_decompression_zlib_ctx_t  *zctx;

    zctx = (ngx_http_decompression_zlib_ctx_t *) ctx;

    if (zctx->initialized) {
        inflateEnd(&zctx->stream);
        zctx->initialized = 0;
    }
}


static void *
ngx_http_decompression_zlib_alloc(void *opaque, u_int items, u_int size)
{
    ngx_http_decompression_ctx_t  *ctx = opaque;

    if (size && items > NGX_MAX_SIZE_T_VALUE / size) {
        return NULL;
    }

    return  ngx_alloc((size_t) items * size, ctx->request->connection->log);
}


static void
ngx_http_decompression_zlib_free(void *opaque, void *address)
{
    ngx_free(address);
}


#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI_DECODER)

static ngx_int_t
ngx_http_decompression_brotli_init(ngx_http_decompression_ctx_t *ctx)
{
    ngx_http_decompression_brotli_ctx_t  *bctx;

    bctx = (ngx_http_decompression_brotli_ctx_t *) ctx;
    bctx->state = BrotliDecoderCreateInstance(NULL, NULL, NULL);

    return  bctx->state ? NGX_OK : NGX_ERROR;
}


static ngx_int_t
ngx_http_decompression_brotli_process(ngx_http_decompression_ctx_t *ctx)
{
    size_t                                input, output, available_in;
    size_t                                available_out;
    const uint8_t                        *next_in;
    uint8_t                              *next_out;
    BrotliDecoderResult                   rc;
    ngx_http_decompression_brotli_ctx_t  *bctx;

    bctx = (ngx_http_decompression_brotli_ctx_t *) ctx;

    if (ctx->complete) {
        if (ngx_buf_size(ctx->in_buf)) {
            ngx_log_error(NGX_LOG_ERR, ctx->request->connection->log, 0,
                          "trailing data in br response");
            return NGX_ERROR;
        }

        return NGX_OK;
    }

    input = ngx_min(ngx_buf_size(ctx->in_buf), NGX_HTTP_DECOMPRESSION_CHUNK);
    output = ngx_min(ctx->out_buf->end - ctx->out_buf->last,
                     NGX_HTTP_DECOMPRESSION_CHUNK);
    available_in = input;
    available_out = output;
    next_in = input ? ctx->in_buf->pos : NULL;
    next_out = ctx->out_buf->last;

    rc = BrotliDecoderDecompressStream(bctx->state, &available_in, &next_in,
                                      &available_out, &next_out, NULL);
    if (input != available_in) {
        ctx->in_buf->pos += input - available_in;
        ctx->bytes_in += input - available_in;
    }

    ctx->out_buf->last += output - available_out;
    ctx->bytes_out += output - available_out;

    if (rc == BROTLI_DECODER_RESULT_ERROR) {
        ngx_log_error(NGX_LOG_ERR, ctx->request->connection->log, 0,
                      "BrotliDecoderDecompressStream() failed: %s",
                      BrotliDecoderErrorString(BrotliDecoderGetErrorCode(
                                                                 bctx->state)));
        return NGX_ERROR;
    }

    ctx->complete = (rc == BROTLI_DECODER_RESULT_SUCCESS);

    return ngx_http_decompression_result(ctx,
                                 rc == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT);
}


static void
ngx_http_decompression_brotli_close(ngx_http_decompression_ctx_t *ctx)
{
    ngx_http_decompression_brotli_ctx_t  *bctx;

    bctx = (ngx_http_decompression_brotli_ctx_t *) ctx;

    if (bctx->state) {
        BrotliDecoderDestroyInstance(bctx->state);
        bctx->state = NULL;
    }
}

#endif


#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD_DECODER)

static ngx_int_t
ngx_http_decompression_zstd_init(ngx_http_decompression_ctx_t *ctx)
{
    ngx_http_decompression_zstd_ctx_t  *zctx;

    zctx = (ngx_http_decompression_zstd_ctx_t *) ctx;
    zctx->state = ZSTD_createDCtx();

    return  zctx->state ? NGX_OK : NGX_ERROR;
}


static ngx_int_t
ngx_http_decompression_zstd_process(ngx_http_decompression_ctx_t *ctx)
{
    size_t                              rc;
    ZSTD_inBuffer                       in;
    ZSTD_outBuffer                      out;
    ngx_http_decompression_zstd_ctx_t  *zctx;

    zctx = (ngx_http_decompression_zstd_ctx_t *) ctx;

    if (ctx->complete && ngx_buf_size(ctx->in_buf) == 0) {
        return NGX_OK;
    }

    in.src = ctx->in_buf->pos;
    in.size = ngx_min(ngx_buf_size(ctx->in_buf), NGX_HTTP_DECOMPRESSION_CHUNK);
    in.pos = 0;
    out.dst = ctx->out_buf->last;
    out.size = ngx_min(ctx->out_buf->end - ctx->out_buf->last,
                      NGX_HTTP_DECOMPRESSION_CHUNK);
    out.pos = 0;

    rc = ZSTD_decompressStream(zctx->state, &out, &in);

    if (in.pos) {
        ctx->in_buf->pos += in.pos;
        ctx->bytes_in += in.pos;
    }

    ctx->out_buf->last += out.pos;
    ctx->bytes_out += out.pos;

    if (ZSTD_isError(rc)) {
        ngx_log_error(NGX_LOG_ERR, ctx->request->connection->log, 0,
                      "ZSTD_decompressStream() failed: %s",
                      ZSTD_getErrorName(rc));
        return NGX_ERROR;
    }

    ctx->complete = (rc == 0);

    return ngx_http_decompression_result(ctx,
                                    !ctx->complete && out.pos == out.size);
}


static void
ngx_http_decompression_zstd_close(ngx_http_decompression_ctx_t *ctx)
{
    ngx_http_decompression_zstd_ctx_t  *zctx;

    zctx = (ngx_http_decompression_zstd_ctx_t *) ctx;

    if (zctx->state) {
        ZSTD_freeDCtx(zctx->state);
        zctx->state = NULL;
    }
}

#endif


static void *
ngx_http_decompression_create_conf(ngx_conf_t *cf)
{
    ngx_http_decompression_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_decompression_conf_t));
    if (conf == NULL) {
        return NULL;
    }

#if (NGX_EXPR)
    conf->enable = NGX_CONF_UNSET_PTR;
    conf->force = NGX_CONF_UNSET_PTR;
#else
    conf->enable = NGX_CONF_UNSET;
    conf->force = NGX_CONF_UNSET;
#endif
    conf->encodings = NGX_CONF_UNSET_PTR;

    return  conf;
}


static char *
ngx_http_decompression_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_decompression_conf_t  *prev = parent;
    ngx_http_decompression_conf_t  *conf = child;

#if (NGX_EXPR)
    if (ngx_conf_merge_expr_flag_value(cf, &conf->enable, prev->enable, 0)
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    if (ngx_conf_merge_expr_flag_value(cf, &conf->force, prev->force, 0)
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    if (ngx_conf_merge_expr_ptr_value(cf, &conf->encodings, prev->encodings,
                                     &ngx_http_decompression_default) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }
#else
    ngx_conf_merge_value(conf->enable, prev->enable, 0);
    ngx_conf_merge_value(conf->force, prev->force, 0);
    ngx_conf_merge_ptr_value(conf->encodings, prev->encodings,
                            &ngx_http_decompression_default);
#endif

    ngx_conf_merge_bufs_value(conf->bufs, prev->bufs,
                              (128 * 1024) / ngx_pagesize, ngx_pagesize);

    return  NGX_CONF_OK;
}


static ngx_int_t
ngx_http_decompression_filter_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_decompression_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_decompression_body_filter;

    return  NGX_OK;
}
