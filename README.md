# ngx_http_compression_filter_module

An nginx response filter for gzip, Brotli, Zstd, and deflate compression.

## Example

~~~nginx
http {
    compression on;
    compression_encodings zstd br gzip deflate;
    compression_types text/plain text/css application/javascript application/json;
    compression_min_length 1k;
    compression_proxied any;

    compression_gzip_level 1;
    compression_brotli_level 6;
    compression_zstd_level 1;
    compression_deflate_level 1;

    server {
        listen 80;
        root html;
    }
}
~~~

## Installation

Requires the development files for at least one of the following libraries. Available backends are detected during configuration.

| Library | Encodings | Directives |
| --- | --- | --- |
| zlib | `gzip`, `deflate` | `compression_gzip_*`, `compression_deflate_*` |
| Brotli | `br` | `compression_brotli_*` |
| Zstd 1.4.0 or later | `zstd` | `compression_zstd_*` |

Directives for unavailable backends are not registered, and their encodings cannot be listed in `compression_encodings`.

~~~sh
./configure --add-module=/path/to/ngx_http_compression_filter_module
make
make install
~~~

For a dynamic module, use `--with-compat --add-dynamic-module=/path/to/ngx_http_compression_filter_module` with the matching nginx source and build options, then load `objs/ngx_http_compression_filter_module.so`:

~~~nginx
load_module modules/ngx_http_compression_filter_module.so;
~~~

Library paths are discovered through `pkg-config` when available. Set `PKG_CONFIG_PATH` for a custom installation.

The native gzip, gzip_static, and gunzip modules are optional. To build without them:

~~~sh
./configure --without-http_gzip_module \
    --add-module=/path/to/ngx_http_compression_filter_module
~~~

The module enables the core support needed by its configuration aliases and `gzip_vary`. A dynamic module requires a matching host build; `--with-compat` enables this core support in a host built without the gzip modules.

If the native gzip filter is compiled, set `gzip off` where this module handles dynamic compression.

### Conditional configuration

Directives whose context includes `when` support [ngx_expr_module](https://github.com/HanadaLee/ngx_expr_module) when it is compiled into nginx:

~~~nginx
expr streaming str_eq $http_accept text/event-stream;

when streaming {
    compression off;
}

compression on;
~~~

## Directives

### compression

**Syntax:** `compression on | off;`  
**Default:** `compression off;`  
**Context:** `http`, `server`, `location`, `when`

Enables compression for responses with status 200, 207, 403, or 404. Responses containing `Cache-Control: no-transform` are not compressed.

### compression_encodings

**Syntax:** `compression_encodings encoding ...;`  
**Default:** `compression_encodings gzip;`  
**Context:** `http`, `server`, `location`, `when`

Sets the available encodings and their preference order: `gzip`, `br`, `zstd`, and `deflate`. A list at a lower configuration level replaces the inherited list.

The default is the first compiled encoding in the order `gzip`, `br`, `zstd`.

The `deflate` encoding uses the zlib format.

### compression_ignore_client_qvalue

**Syntax:** `compression_ignore_client_qvalue on | off;`  
**Default:** `compression_ignore_client_qvalue off;`  
**Context:** `http`, `server`, `location`, `when`

Selects the first accepted encoding in `compression_encodings` without comparing client quality values. Encodings with `q=0` remain excluded.

### compression_types

**Syntax:** `compression_types mime-type ...;`  
**Default:** `compression_types text/html;`  
**Context:** `http`, `server`, `location`

Enables compression for the specified MIME types in addition to `text/html`. The value `*` matches any MIME type.

### compression_min_length

**Syntax:** `compression_min_length length;`  
**Default:** `compression_min_length 20;`  
**Context:** `http`, `server`, `location`, `when`

Sets the minimum response length for compression, determined from the `Content-Length` response header.

### compression_max_length

**Syntax:** `compression_max_length length;`  
**Default:** `compression_max_length 0;`  
**Context:** `http`, `server`, `location`, `when`

Sets the maximum response length for compression, determined from the `Content-Length` response header. The value `0` disables the limit. Responses without a known length are not restricted by either length directive.

### compression_bypass

**Syntax:** `compression_bypass string ...;`  
**Default:** —  
**Context:** `http`, `server`, `location`

Skips compression if any argument is nonempty and not equal to `0`. Arguments can contain variables.

### compression_buffers

**Syntax:** `compression_buffers number size;`  
**Default:** `compression_buffers 32 4k;` / `compression_buffers 16 8k;`  
**Context:** `http`, `server`, `location`

Sets the number and size of output buffers. The default size is one memory page, with a total capacity of 128 kilobytes.

### compression_postpone

**Syntax:** `compression_postpone size;`  
**Default:** `compression_postpone 0;`  
**Context:** `http`, `server`, `location`

Postpones encoder initialization until the specified amount of input has been buffered. A flush or the end of the response starts compression earlier.

### compression_no_buffer

**Syntax:** `compression_no_buffer on | off;`  
**Default:** `compression_no_buffer off;`  
**Context:** `http`, `server`, `location`

Passes partially filled output buffers downstream when the currently available input has been consumed. This does not force an encoder flush.

### compression_http_version

**Syntax:** `compression_http_version 1.0 | 1.1;`  
**Default:** `compression_http_version 1.1;`  
**Context:** `http`, `server`, `location`

Alias of [gzip_http_version](https://nginx.org/en/docs/http/ngx_http_gzip_module.html#gzip_http_version), applied to all supported encodings.

### compression_proxied

**Syntax:** `compression_proxied off | expired | no-cache | no-store | private | no_last_modified | no_etag | auth | any ...;`  
**Default:** `compression_proxied off;`  
**Context:** `http`, `server`, `location`

Alias of [gzip_proxied](https://nginx.org/en/docs/http/ngx_http_gzip_module.html#gzip_proxied), applied to all supported encodings.

### compression_disable

**Syntax:** `compression_disable regex ...;`  
**Default:** —  
**Context:** `http`, `server`, `location`

Alias of [gzip_disable](https://nginx.org/en/docs/http/ngx_http_gzip_module.html#gzip_disable), applied to all supported encodings.

The three aliases share the native directives' configuration and inheritance. Changes also affect native modules that use these settings.

### compression_gzip_level

**Syntax:** `compression_gzip_level level;`  
**Default:** `compression_gzip_level 1;`  
**Context:** `http`, `server`, `location`, `when`

Sets the gzip compression level, from `1` to `9`.

### compression_gzip_window

**Syntax:** `compression_gzip_window size;`  
**Default:** `compression_gzip_window 32k;`  
**Context:** `http`, `server`, `location`

Sets the gzip window size, a power of two from `512` to `32k`.

### compression_gzip_hash

**Syntax:** `compression_gzip_hash size;`  
**Default:** `compression_gzip_hash 64k;`  
**Context:** `http`, `server`, `location`

Sets the gzip hash table size, a power of two from `512` to `128k`.

### compression_brotli_level

**Syntax:** `compression_brotli_level level;`  
**Default:** `compression_brotli_level 6;`  
**Context:** `http`, `server`, `location`, `when`

Sets the Brotli compression level, from `0` to `11`.

### compression_brotli_window

**Syntax:** `compression_brotli_window size;`  
**Default:** `compression_brotli_window 512k;`  
**Context:** `http`, `server`, `location`

Sets the Brotli window size, a power of two from `1k` to `16m`.

### compression_zstd_level

**Syntax:** `compression_zstd_level level;`  
**Default:** `compression_zstd_level 1;`  
**Context:** `http`, `server`, `location`, `when`

Sets the Zstd compression level. The range is determined by libzstd; negative levels are supported and `0` is excluded.

### compression_deflate_level

**Syntax:** `compression_deflate_level level;`  
**Default:** `compression_deflate_level 1;`  
**Context:** `http`, `server`, `location`, `when`

Sets the deflate compression level, from `1` to `9`.

### compression_deflate_window

**Syntax:** `compression_deflate_window size;`  
**Default:** `compression_deflate_window 32k;`  
**Context:** `http`, `server`, `location`

Sets the deflate window size, a power of two from `512` to `32k`.

### compression_deflate_hash

**Syntax:** `compression_deflate_hash size;`  
**Default:** `compression_deflate_hash 64k;`  
**Context:** `http`, `server`, `location`

Sets the deflate hash table size, a power of two from `512` to `128k`.

## Negotiation

The module reads the effective `Accept-Encoding` request header. By default, the highest client quality value wins; equal values use the configured encoding order. An explicit `identity` preference is respected. A wildcard applies only to encodings without an explicit entry.

Missing or empty `Accept-Encoding` selects an uncompressed response. Repeated encodings use the first occurrence; invalid quality values make that entry unavailable. When compression is applicable but neither a configured encoding nor identity is acceptable, the response is `406`.

## Variables

### $compression_ratio

Original size divided by compressed size, with two decimal places; available after successful compression.

Remove any existing `map` or `var` definition of `$compression_ratio` before enabling this module.

## Compatibility

Only main requests are compressed. HEAD responses and responses with an existing nonempty `Content-Encoding` are passed through.

`gzip_vary on` enables native `Vary: Accept-Encoding` handling. [ngx_http_compression_vary_filter_module](https://github.com/HanadaLee/ngx_http_compression_vary_filter_module) can manage Vary separately. [ngx_http_compression_normalize_module](https://github.com/HanadaLee/ngx_http_compression_normalize_module) can independently normalize Accept-Encoding.

## Testing

Tests use [nginx-tests](https://github.com/nginx/nginx-tests) and decode responses with the codec libraries:

~~~sh
TEST_NGINX_BINARY=/path/to/nginx \
PERL5LIB=/path/to/nginx-tests/lib \
prove -v t/*.t
~~~

Set `TEST_NGINX_EXPR=1` for conditional configuration tests, `TEST_NGINX_COMPRESSION_AUX=1` when normalize and vary are compiled, and `TEST_NGINX_OPENRESTY=1` for Lua response tests.

For a partial build tested directly with `prove`, set `TEST_NGINX_COMPRESSION_ENCODINGS` to its available encodings, separated by spaces, and `TEST_NGINX_GZIP=0` when the native gzip modules are absent.

## License

BSD-2-Clause. See [LICENSE](LICENSE) for the original copyright notices.
