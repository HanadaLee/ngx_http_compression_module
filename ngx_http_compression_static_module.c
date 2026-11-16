
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 * Copyright (C) Google Inc.
 * Copyright (C) Alex Zhang
 * Copyright (C) Hanada
 */


#include <ngx_http_compression.h>


#define NGX_HTTP_COMPRESSION_STATIC_OFF       0
#define NGX_HTTP_COMPRESSION_STATIC_ON        1
#define NGX_HTTP_COMPRESSION_STATIC_ALWAYS    2


typedef struct {
#if (NGX_EXPR)
    ngx_array_t                       *enable;
    ngx_array_t                       *encodings;
    ngx_array_t                       *ignore_qvalue;
#else
    ngx_uint_t                         enable;
    ngx_http_compression_encodings_t  *encodings;
    ngx_flag_t                         ignore_qvalue;
#endif
} ngx_http_compression_static_conf_t;


static ngx_int_t ngx_http_compression_static_handler(ngx_http_request_t *r);
static void *ngx_http_compression_static_create_conf(ngx_conf_t *cf);
static char *ngx_http_compression_static_merge_conf(ngx_conf_t *cf,
    void *parent, void *child);
static ngx_int_t ngx_http_compression_static_init(ngx_conf_t *cf);


static ngx_conf_enum_t  ngx_http_compression_static_values[] = {
    { ngx_string("off"), NGX_HTTP_COMPRESSION_STATIC_OFF },
    { ngx_string("on"), NGX_HTTP_COMPRESSION_STATIC_ON },
    { ngx_string("always"), NGX_HTTP_COMPRESSION_STATIC_ALWAYS },
    { ngx_null_string, 0 }
};

static ngx_str_t  ngx_http_compression_static_suffixes[] = {
    ngx_string(".gz"),
    ngx_string(".br"),
    ngx_string(".zst"),
    ngx_string(".deflate")
};


static ngx_command_t  ngx_http_compression_static_commands[] = {

    { ngx_string("compression_static"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_TAKE1,
#if (NGX_EXPR)
      ngx_conf_set_conditional_enum_slot,
#else
      ngx_conf_set_enum_slot,
#endif
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_static_conf_t, enable),
      ngx_http_compression_static_values },

    { ngx_string("compression_static_encodings"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF
#if (NGX_EXPR)
                        |NGX_HTTP_MAIN_WHEN_CONF|NGX_HTTP_SRV_WHEN_CONF
                        |NGX_HTTP_LOC_WHEN_CONF
#endif
                        |NGX_CONF_1MORE,
      ngx_http_compression_encodings,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_compression_static_conf_t, encodings),
      &ngx_http_compression_encode_mask },

    { ngx_string("compression_static_ignore_client_qvalue"),
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
      offsetof(ngx_http_compression_static_conf_t, ignore_qvalue),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_compression_static_module_ctx = {
    NULL,                                     /* preconfiguration */
    ngx_http_compression_static_init,         /* postconfiguration */

    NULL,                                     /* create main configuration */
    NULL,                                     /* init main configuration */

    NULL,                                     /* create server configuration */
    NULL,                                     /* merge server configuration */

    ngx_http_compression_static_create_conf,  /* create location conf */
    ngx_http_compression_static_merge_conf    /* merge location conf */
};


ngx_module_t  ngx_http_compression_static_module = {
    NGX_MODULE_V1,
    &ngx_http_compression_static_module_ctx,  /* module context */
    ngx_http_compression_static_commands,     /* module directives */
    NGX_HTTP_MODULE,                          /* module type */
    NULL,                                     /* init master */
    NULL,                                     /* init module */
    NULL,                                     /* init process */
    NULL,                                     /* init thread */
    NULL,                                     /* exit thread */
    NULL,                                     /* exit process */
    NULL,                                     /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_http_compression_static_handler(ngx_http_request_t *r)
{
    u_char                              *p;
    size_t                               root, len;
    ngx_int_t                            rc, encoding;
    ngx_uint_t                           enable, mask, i, level;
    ngx_flag_t                           ignore;
    ngx_str_t                            path, *suffix;
    ngx_log_t                           *log;
    ngx_buf_t                           *b;
    ngx_chain_t                          out;
    ngx_table_elt_t                     *h;
    ngx_open_file_info_t                 of;
    ngx_http_core_loc_conf_t            *clcf;
    ngx_http_compression_encodings_t    *encodings;
    ngx_http_compression_static_conf_t  *conf;

    if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_HEAD))
        || r->uri.len == 0 || r->uri.data[r->uri.len - 1] == '/')
    {
        return NGX_DECLINED;
    }

    conf = ngx_http_get_module_loc_conf(r, ngx_http_compression_static_module);

#if (NGX_EXPR)
    enable = ngx_http_get_expr_enum_value(r, conf->enable);
#else
    enable = conf->enable;
#endif

    if (enable == NGX_HTTP_COMPRESSION_STATIC_OFF) {
        return NGX_DECLINED;
    }

#if (NGX_EXPR)
    encodings = ngx_http_get_expr_ptr_value(r, conf->encodings);
    ignore = ngx_http_get_expr_flag_value(r, conf->ignore_qvalue);
#else
    encodings = conf->encodings;
    ignore = conf->ignore_qvalue;
#endif

    if (enable == NGX_HTTP_COMPRESSION_STATIC_ON) {
        if (ngx_http_compression_ok(r) != NGX_OK) {
            return NGX_DECLINED;
        }

        r->gzip_vary = 1;
    }

    p = ngx_http_map_uri_to_path(r, &path, &root, sizeof(".deflate") - 1);
    if (p == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    len = p - path.data;
    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
    log = r->connection->log;
    mask = ngx_http_compression_encode_mask;

    for ( ;; ) {
        if (enable == NGX_HTTP_COMPRESSION_STATIC_ALWAYS) {
            encoding = NGX_DECLINED;

            for (i = 0; i < encodings->nelts; i++) {
                if (mask & (1 << encodings->encodings[i])) {
                    encoding = encodings->encodings[i];
                    break;
                }
            }

        } else {
            encoding = ngx_http_compression_select(r, encodings, ignore, mask);
        }

        if (encoding < 0 || encoding == NGX_HTTP_COMPRESSION_NOT_ACCEPTABLE) {
            return NGX_DECLINED;
        }

        mask &= ~(1 << encoding);
        suffix = &ngx_http_compression_static_suffixes[encoding];
        p = ngx_cpymem(path.data + len, suffix->data, suffix->len);
        *p = '\0';
        path.len = p - path.data;

        ngx_memzero(&of, sizeof(ngx_open_file_info_t));
        of.read_ahead = clcf->read_ahead;
        of.directio = clcf->directio;
        of.valid = clcf->open_file_cache_valid;
        of.min_uses = clcf->open_file_cache_min_uses;
        of.errors = clcf->open_file_cache_errors;
        of.events = clcf->open_file_cache_events;

        if (ngx_http_set_disable_symlinks(r, clcf, &path, &of) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        if (ngx_open_cached_file(clcf->open_file_cache, &path, &of, r->pool)
            != NGX_OK)
        {
            switch (of.err) {

            case 0:
                return NGX_HTTP_INTERNAL_SERVER_ERROR;

            case NGX_ENOENT:
            case NGX_ENOTDIR:
            case NGX_ENAMETOOLONG:
                continue;

            case NGX_EACCES:
#if (NGX_HAVE_OPENAT)
            case NGX_EMLINK:
            case NGX_ELOOP:
#endif
                level = NGX_LOG_ERR;
                break;

            default:
                level = NGX_LOG_CRIT;
                break;
            }

            ngx_log_error(level, log, of.err,
                          "%s \"%s\" failed", of.failed, path.data);
            return NGX_DECLINED;
        }

        if (of.is_dir) {
            continue;
        }

#if !(NGX_WIN32)
        if (!of.is_file) {
            ngx_log_error(NGX_LOG_CRIT, log, 0,
                          "\"%s\" is not a regular file", path.data);
            return NGX_HTTP_NOT_FOUND;
        }
#endif

        break;
    }

    r->root_tested = !r->error_page;

    rc = ngx_http_discard_request_body(r);

    if (rc != NGX_OK) {
        return rc;
    }

    log->action = "sending response to client";

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = of.size;
    r->headers_out.last_modified_time = of.mtime;

    if (ngx_http_set_etag(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (ngx_http_set_content_type(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    h = ngx_list_push(&r->headers_out.headers);
    if (h == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    h->hash = 1;
    h->next = NULL;
    ngx_str_set(&h->key, "Content-Encoding");
    h->value = ngx_http_compression_names[encoding];
    r->headers_out.content_encoding = h;

    r->allow_ranges = 1;

    /* we need to allocate all before the header would be sent */

    b = ngx_calloc_buf(r->pool);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b->file = ngx_pcalloc(r->pool, sizeof(ngx_file_t));
    if (b->file == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    b->file_pos = 0;
    b->file_last = of.size;

    b->in_file = b->file_last ? 1 : 0;
    b->last_buf = (r == r->main) ? 1 : 0;
    b->last_in_chain = 1;
    b->sync = (b->last_buf || b->in_file) ? 0 : 1;

    b->file->fd = of.fd;
    b->file->name = path;
    b->file->log = log;
    b->file->directio = of.is_directio;

    out.buf = b;
    out.next = NULL;

    return  ngx_http_output_filter(r, &out);
}


static void *
ngx_http_compression_static_create_conf(ngx_conf_t *cf)
{
    ngx_http_compression_static_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_compression_static_conf_t));
    if (conf == NULL) {
        return NULL;
    }

#if (NGX_EXPR)
    conf->enable = NGX_CONF_UNSET_PTR;
    conf->ignore_qvalue = NGX_CONF_UNSET_PTR;
#else
    conf->enable = NGX_CONF_UNSET_UINT;
    conf->ignore_qvalue = NGX_CONF_UNSET;
#endif
    conf->encodings = NGX_CONF_UNSET_PTR;

    return  conf;
}


static char *
ngx_http_compression_static_merge_conf(ngx_conf_t *cf, void *parent,
    void *child)
{
    ngx_http_compression_static_conf_t  *prev = parent;
    ngx_http_compression_static_conf_t  *conf = child;

#if (NGX_EXPR)
    if (ngx_conf_merge_expr_enum_value(cf, &conf->enable, prev->enable,
                                      NGX_HTTP_COMPRESSION_STATIC_OFF)
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    if (ngx_conf_merge_expr_flag_value(cf, &conf->ignore_qvalue,
                                      prev->ignore_qvalue, 0) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    if (ngx_conf_merge_expr_ptr_value(cf, &conf->encodings, prev->encodings,
                                     &ngx_http_compression_default) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }
#else
    ngx_conf_merge_uint_value(conf->enable, prev->enable,
                             NGX_HTTP_COMPRESSION_STATIC_OFF);
    ngx_conf_merge_value(conf->ignore_qvalue, prev->ignore_qvalue, 0);
    ngx_conf_merge_ptr_value(conf->encodings, prev->encodings,
                            &ngx_http_compression_default);
#endif

    return  NGX_CONF_OK;
}


static ngx_int_t
ngx_http_compression_static_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    h = ngx_array_push(&cmcf->phases[NGX_HTTP_CONTENT_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_compression_static_handler;

    return  NGX_OK;
}
