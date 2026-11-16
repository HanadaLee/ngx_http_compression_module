/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Nginx, Inc.
 * Copyright (C) Hanada
 */


#ifndef _NGX_HTTP_COMPRESSION_H_INCLUDED_
#define _NGX_HTTP_COMPRESSION_H_INCLUDED_


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

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
#define NGX_HTTP_COMPRESSION_NOT_ACCEPTABLE       406


typedef struct {
    ngx_uint_t  nelts;
    ngx_uint_t  encodings[NGX_HTTP_COMPRESSION_ENCODINGS];
} ngx_http_compression_encodings_t;


extern ngx_str_t                           ngx_http_compression_names[];
extern ngx_http_compression_encodings_t    ngx_http_compression_default;
extern ngx_http_compression_encodings_t    ngx_http_decompression_default;
extern ngx_uint_t                          ngx_http_compression_encode_mask;
extern ngx_uint_t                          ngx_http_compression_decode_mask;

ngx_int_t ngx_http_compression_ok(ngx_http_request_t *r);
ngx_int_t ngx_http_compression_cache_control(ngx_http_request_t *r);
ngx_int_t ngx_http_compression_parse(ngx_http_request_t *r, ngx_int_t *quality);
ngx_int_t ngx_http_compression_select(ngx_http_request_t *r,
    ngx_http_compression_encodings_t *encodings, ngx_flag_t ignore,
    ngx_uint_t mask);
ngx_int_t ngx_http_compression_accepted(ngx_http_request_t *r,
    ngx_uint_t encoding);
char *ngx_http_compression_alias(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
char *ngx_http_compression_encodings(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);


#endif /* _NGX_HTTP_COMPRESSION_H_INCLUDED_ */
