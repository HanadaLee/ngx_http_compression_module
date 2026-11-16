/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 * Copyright (C) Hanada
 */


#include <ngx_http_compression.h>


static ngx_int_t ngx_http_compression_qvalue(u_char *p, u_char *last);


ngx_uint_t ngx_http_compression_encode_mask =
#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI)
    (1 << NGX_HTTP_COMPRESSION_BROTLI) |
#endif
#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD)
    (1 << NGX_HTTP_COMPRESSION_ZSTD) |
#endif
    (1 << NGX_HTTP_COMPRESSION_GZIP) | (1 << NGX_HTTP_COMPRESSION_DEFLATE);

ngx_uint_t ngx_http_compression_decode_mask =
#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI_DECODER)
    (1 << NGX_HTTP_COMPRESSION_BROTLI) |
#endif
#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD_DECODER)
    (1 << NGX_HTTP_COMPRESSION_ZSTD) |
#endif
    (1 << NGX_HTTP_COMPRESSION_GZIP) | (1 << NGX_HTTP_COMPRESSION_DEFLATE);

ngx_str_t  ngx_http_compression_names[] = {
    ngx_string("gzip"),
    ngx_string("br"),
    ngx_string("zstd"),
    ngx_string("deflate"),
    ngx_string("identity"),
    ngx_string("*")
};

ngx_http_compression_encodings_t  ngx_http_compression_default = {
    1, { NGX_HTTP_COMPRESSION_GZIP, 0, 0, 0 }
};

ngx_http_compression_encodings_t  ngx_http_decompression_default = {
#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI_DECODER)
    1 +
#endif
#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD_DECODER)
    1 +
#endif
    2, {
        NGX_HTTP_COMPRESSION_GZIP,
        NGX_HTTP_COMPRESSION_DEFLATE,
#if (NGX_HTTP_COMPRESSION_HAVE_BROTLI_DECODER)
        NGX_HTTP_COMPRESSION_BROTLI,
#endif
#if (NGX_HTTP_COMPRESSION_HAVE_ZSTD_DECODER)
        NGX_HTTP_COMPRESSION_ZSTD,
#endif
    }
};


ngx_int_t
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


ngx_int_t
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


char *
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

    return NGX_CONF_ERROR;
}


ngx_int_t
ngx_http_compression_parse(ngx_http_request_t *r, ngx_int_t *quality)
{
    u_char           *p, *last, *start, *end, *name_end;
    ngx_uint_t        i;
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

    return NGX_OK;
}


ngx_int_t
ngx_http_compression_select(ngx_http_request_t *r,
    ngx_http_compression_encodings_t *encodings, ngx_flag_t ignore,
    ngx_uint_t mask)
{
    ngx_int_t   best, weight, selected, identity;
    ngx_int_t   quality[NGX_HTTP_COMPRESSION_CODINGS];
    ngx_uint_t  i, code;

    if (ngx_http_compression_parse(r, quality) != NGX_OK) {
        return NGX_DECLINED;
    }

    best = 0;
    selected = NGX_DECLINED;

    for (i = 0; i < encodings->nelts; i++) {
        code = encodings->encodings[i];

        if (!(mask & (1 << code))) {
            continue;
        }

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


ngx_int_t
ngx_http_compression_accepted(ngx_http_request_t *r, ngx_uint_t encoding)
{
    ngx_int_t  quality[NGX_HTTP_COMPRESSION_CODINGS];

    if (ngx_http_compression_parse(r, quality) != NGX_OK) {
        return NGX_DECLINED;
    }

    if (quality[encoding] == -1) {
        quality[encoding] = quality[NGX_HTTP_COMPRESSION_WILDCARD];
    }

    return quality[encoding] > 0 ? NGX_OK : NGX_DECLINED;
}


char *
ngx_http_compression_encodings(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    char  *p = conf;

    ngx_uint_t                          i, j, mask, available;
    ngx_str_t                          *value;
    ngx_http_compression_encodings_t   *encodings;
#if (NGX_EXPR)
    ngx_array_t                       **field;
    ngx_expr_when_id_t                  id;
    ngx_conf_expr_ptr_ctx_t            *ctx;
#else
    ngx_http_compression_encodings_t  **field;
#endif

    field = (void *) (p + cmd->offset);
    available = *(ngx_uint_t *) cmd->post;

    if (cf->args->nelts > NGX_HTTP_COMPRESSION_ENCODINGS + 1) {
        return "has too many encodings";
    }

#if (NGX_EXPR)
    if (*field == NGX_CONF_UNSET_PTR) {
        *field = ngx_array_create(cf->pool, 2,
                                  sizeof(ngx_conf_expr_ptr_ctx_t));
        if (*field == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    id = ngx_expr_get_associated_when_id(cf);
    ctx = ngx_expr_find_ctx(*field, id,
                            sizeof(ngx_conf_expr_ptr_ctx_t),
                            offsetof(ngx_conf_expr_ptr_ctx_t, expr_id));
    if (ctx) {
        return "is duplicate";
    }
#else
    if (*field != NGX_CONF_UNSET_PTR) {
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

        if (!(available & (1 << j))) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "encoding \"%V\" is not available", &value[i]);
            return NGX_CONF_ERROR;
        }

        mask |= 1 << j;
        encodings->encodings[encodings->nelts++] = j;
    }

#if (NGX_EXPR)
    ctx = ngx_array_push(*field);
    if (ctx == NULL) {
        return NGX_CONF_ERROR;
    }

    ctx->expr_id = id;
    ctx->value = encodings;
#else
    *field = encodings;
#endif

    return NGX_CONF_OK;
}
