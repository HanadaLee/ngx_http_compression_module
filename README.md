# ngx_http_compression_module

nginx modules for dynamic compression, precompressed files, and response decompression using gzip, Brotli, Zstd, and deflate.

| Module | Function |
| --- | --- |
| `ngx_http_compression_filter_module` | Compresses response bodies. |
| `ngx_http_compression_static_module` | Serves precompressed files. |
| `ngx_http_decompression_filter_module` | Decompresses encoded responses. |
| `ngx_http_compression_vary_filter_module` | Merges Vary response header fields. |
| `ngx_http_compression_normalize_module` | Normalizes the Accept-Encoding request header field. |

## Example

```nginx
http {
    compression_vary on;
    compression on;
    compression_encodings zstd br gzip deflate;
    compression_types text/plain text/css application/javascript application/json;
    compression_min_length 1k;
    compression_proxied any;

    compression_gzip_level 1;
    compression_brotli_level 1;
    compression_zstd_level 1;
    compression_deflate_level 1;

    server {
        listen 80;
        root html;

        location /assets/ {
            compression_static on;
            compression_static_encodings zstd br gzip deflate;
        }

        location /proxy/ {
            decompression on;
            proxy_pass http://backend;
        }
    }
}
```

## Installation

Requires the zlib development files. Brotli and Zstd encoding and decoding libraries are detected separately during configuration.

| Library | Encodings | Directives |
| --- | --- | --- |
| zlib | `gzip`, `deflate` | `compression_gzip_*`, `compression_deflate_*` |
| Brotli | `br` | `compression_brotli_*` |
| Zstd 1.4.0 or later | `zstd` | `compression_zstd_*` |

Directives for unavailable backends are not registered, and their encodings cannot be listed in `compression_encodings`.

```sh
./configure --add-module=/path/to/ngx_http_compression_module
make
make install
```

For dynamic modules, use `--with-compat --add-dynamic-module=/path/to/ngx_http_compression_module` with the matching nginx source and build options. The five generated modules can be loaded independently:

```nginx
load_module modules/ngx_http_compression_filter_module.so;
load_module modules/ngx_http_compression_static_module.so;
load_module modules/ngx_http_decompression_filter_module.so;
load_module modules/ngx_http_compression_vary_filter_module.so;
load_module modules/ngx_http_compression_normalize_module.so;
```

Library paths are discovered through `pkg-config` when available. Set `PKG_CONFIG_PATH` for a custom installation.

The native gzip, gzip_static, and gunzip modules are optional. To build without them:

```sh
./configure --without-http_gzip_module \
    --add-module=/path/to/ngx_http_compression_module
```

The module enables the core support needed by its configuration aliases and `gzip_vary`. A dynamic module requires a matching host build; `--with-compat` enables this core support in a host built without the gzip modules.

If the native gzip filter is compiled, set `gzip off` where this module handles dynamic compression.

### Conditional configuration

Directives whose context includes `when` support [ngx_expr_module](https://github.com/HanadaLee/ngx_expr_module) when it is compiled into nginx:

```nginx
expr streaming str_eq $http_accept text/event-stream;

when streaming {
    compression off;
}

compression on;
```

## Directives

### compression

**Syntax:** `compression on | off;`
**Default:** `compression off;`
**Context:** `http`, `server`, `location`, `when`

Enables or disables compression of responses.

### compression_encodings

**Syntax:** `compression_encodings encoding ...;`
**Default:** `compression_encodings gzip;`
**Context:** `http`, `server`, `location`, `when`

Sets the available encodings and their preference order: `gzip`, `br`, `zstd`, and `deflate`. A list at a lower configuration level replaces the inherited list.

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

Enables compression of responses for the specified MIME types in addition to `text/html`. The special value `*` matches any MIME type. Responses with the `text/html` type are always compressed.

### compression_min_length

**Syntax:** `compression_min_length length;`
**Default:** `compression_min_length 20;`
**Context:** `http`, `server`, `location`, `when`

Sets the minimum length of a response that will be compressed. The length is determined only from the `Content-Length` response header field.

### compression_max_length

**Syntax:** `compression_max_length length;`
**Default:** `compression_max_length 0;`
**Context:** `http`, `server`, `location`, `when`

Sets the maximum length of a response that will be compressed. The length is determined only from the `Content-Length` response header field. The zero value disables the limit.

### compression_bypass

**Syntax:** `compression_bypass string ...;`
**Default:** —
**Context:** `http`, `server`, `location`

Skips compression if any argument is nonempty and not equal to `0`. Arguments can contain variables.

### compression_buffers

**Syntax:** `compression_buffers number size;`
**Default:** `compression_buffers 32 4k|16 8k;`
**Context:** `http`, `server`, `location`

Sets the `number` and `size` of buffers used to compress a response. By default, the buffer size is equal to one memory page. This is either 4K or 8K, depending on a platform.

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

### compression_gzip_level

**Syntax:** `compression_gzip_level level;`
**Default:** `compression_gzip_level 1;`
**Context:** `http`, `server`, `location`, `when`

Sets a gzip compression `level` of a response. Acceptable values are in the range from 1 to 9.

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
**Default:** `compression_brotli_level 1;`
**Context:** `http`, `server`, `location`, `when`

Sets a Brotli compression `level` of a response. Acceptable values are in the range from 0 to 11.

### compression_brotli_window

**Syntax:** `compression_brotli_window size;`
**Default:** `compression_brotli_window 512k;`
**Context:** `http`, `server`, `location`

Sets the Brotli window size, a power of two from `1k` to `16m`.

### compression_zstd_level

**Syntax:** `compression_zstd_level level;`
**Default:** `compression_zstd_level 1;`
**Context:** `http`, `server`, `location`, `when`

Sets a Zstd compression `level` of a response. Acceptable values are determined by libzstd. Negative values are supported; the zero value is not allowed.

### compression_zstd_window

**Syntax:** `compression_zstd_window size;`
**Default:** `compression_zstd_window 512k;`
**Context:** `http`, `server`, `location`

Sets the Zstd window size, a power of two from `1k` to `8m`.

### compression_zstd_target_block_size

**Syntax:** `compression_zstd_target_block_size size;`
**Default:** `compression_zstd_target_block_size 0;`
**Context:** `http`, `server`, `location`

Sets the target compressed block size. This is an approximate target, independent of the output buffer size. The value `0` uses the library default.

Available with libzstd 1.5.6 or later.

### compression_deflate_level

**Syntax:** `compression_deflate_level level;`
**Default:** `compression_deflate_level 1;`
**Context:** `http`, `server`, `location`, `when`

Sets a deflate compression `level` of a response. Acceptable values are in the range from 1 to 9.

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

## Static compression directives

### compression_static

**Syntax:** `compression_static on | off | always;`
**Default:** `compression_static off;`
**Context:** `http`, `server`, `location`, `when`

Enables (`on`) or disables (`off`) checking the existence of precompressed files. The following directives are also taken into account: [compression_http_version](#compression_http_version), [compression_proxied](#compression_proxied), [compression_disable](#compression_disable), and [compression_vary](#compression_vary).

With the `always` value, compressed file is used in all cases, without checking if the client supports it. It is useful if there are no uncompressed files on the disk anyway or the [ngx_http_decompression_filter_module](#decompression) is used.

The files can be compressed using the `gzip`, `brotli`, or `zstd` commands, or any other compatible one. It is recommended that the modification date and time of original and compressed files be the same.

The file suffixes are `.gz`, `.br`, `.zst`, and `.deflate`.

### compression_static_encodings

**Syntax:** `compression_static_encodings encoding ...;`
**Default:** `compression_static_encodings gzip;`
**Context:** `http`, `server`, `location`, `when`

Sets the encodings and their preference order. If a file does not exist, the remaining encodings are checked in order of preference. A list at a lower configuration level replaces the inherited list.

### compression_static_ignore_client_qvalue

**Syntax:** `compression_static_ignore_client_qvalue on | off;`
**Default:** `compression_static_ignore_client_qvalue off;`
**Context:** `http`, `server`, `location`, `when`

Selects the first accepted encoding in `compression_static_encodings` without comparing client quality values. Encodings with `q=0` remain excluded.

## Decompression directives

### decompression

**Syntax:** `decompression on | off;`
**Default:** `decompression off;`
**Context:** `http`, `server`, `location`, `when`

Enables or disables decompression of compressed responses for clients that lack support for the encoding. If enabled, the following directives are also taken into account when determining if clients support the encoding: [compression_http_version](#compression_http_version), [compression_proxied](#compression_proxied), and [compression_disable](#compression_disable). See also the [compression_vary](#compression_vary) directive.

### decompression_force

**Syntax:** `decompression_force on | off;`
**Default:** `decompression_force off;`
**Context:** `http`, `server`, `location`, `when`

Enables or disables decompression of responses regardless of whether the client supports their encoding.

### decompression_encodings

**Syntax:** `decompression_encodings encoding ...;`
**Default:** all encodings with an available decoder
**Context:** `http`, `server`, `location`, `when`

Sets the response encodings eligible for decompression. A list at a lower configuration level replaces the inherited list.

### decompression_buffers

**Syntax:** `decompression_buffers number size;`
**Default:** `decompression_buffers 32 4k|16 8k;`
**Context:** `http`, `server`, `location`

Sets the `number` and `size` of buffers used to decompress a response. By default, the buffer size is equal to one memory page. This is either 4K or 8K, depending on a platform.

## Normalization directives

### compression_normalize_accept_encoding

**Syntax:** `compression_normalize_accept_encoding combination ...;`
**Syntax:** `compression_normalize_accept_encoding off;`
**Default:** `compression_normalize_accept_encoding off;`
**Context:** `http`, `server`, `location`, `when`

Replaces the `Accept-Encoding` request header field with the first configured combination accepted by the client. Encodings within a combination are separated by commas. Encoding names are compared without regard to case. Encodings with `q=0` are excluded; other quality values do not affect selection. If no combination matches, the header field is removed.

For example:

```nginx
compression_normalize_accept_encoding gzip,br,zstd gzip,br zstd br gzip;
```

The `off` parameter disables normalization.

## Vary directives

### compression_vary

**Syntax:** `compression_vary on | off;`
**Default:** `compression_vary off;`
**Context:** `http`, `server`, `location`

Enables or disables inserting the `Vary: Accept-Encoding` response header field if the directives [compression](#compression), [compression_static](#compression_static), or [decompression](#decompression) are active.

Existing `Vary` response header fields are merged and duplicate values are removed. The [gzip_vary](https://nginx.org/en/docs/http/ngx_http_gzip_module.html#gzip_vary) directive should be set to `off`.

## Variables

### $compression_original_accept_encoding

The original value of the `Accept-Encoding` request header field.

### $compression_ratio

Original size divided by compressed size, with two decimal places; available after successful compression.

Remove any existing `map` or `var` definition of `$compression_ratio` before enabling this module.

## Compatibility

Only main requests are compressed. HEAD responses and responses with an existing nonempty `Content-Encoding` are passed through.

Dynamic compression applies to responses with status 200, 207, 403, or 404. Responses containing `Cache-Control: no-transform` are neither compressed nor decompressed. Decompression also skips partial responses and stacked content encodings.

The deflate decoder recognizes zlib headers and otherwise accepts raw deflate for compatibility. Concatenated gzip members and Zstd frames are supported. Invalid or incomplete compressed streams terminate the response.

The dynamic, static, and decompression modules are registered alongside nginx's gzip filter, gzip static module, and gunzip filter, respectively. Decompression precedes content filters such as `sub_filter`; dynamic compression follows them. The Vary filter runs after compression and response header additions. Static files retain normal file caching, conditional request, and encoded-byte range handling.

## Testing

Tests use [nginx-tests](https://github.com/nginx/nginx-tests) and decode responses with the codec libraries:

```sh
TEST_NGINX_BINARY=/path/to/nginx \
PERL5LIB=/path/to/nginx-tests/lib \
prove -v t/*.t
```

Set `TEST_NGINX_EXPR=1` for conditional configuration tests and `TEST_NGINX_OPENRESTY=1` for Lua response tests.

For a partial build tested directly with `prove`, set `TEST_NGINX_COMPRESSION_ENCODINGS` to its available encodings, separated by spaces, and `TEST_NGINX_GZIP=0` when the native gzip modules are absent. Set `TEST_NGINX_ZSTD_TARGET_BLOCK_SIZE=1` when the target block size directive is available. The build runner sets these automatically.

Set `TEST_NGINX_DECOMPRESSION_ENCODINGS` separately when decoder availability differs. Dynamic builds also check that each module can be loaded independently, and test reversed loading order.

`util/run-tests.sh` builds nginx and runs the suite using sources under `.ci/`. The workflow covers static and dynamic builds, optional expr support, library combinations without the native gzip modules, and sanitizers. Library combinations use a compiler wrapper to make excluded libraries fail the normal configuration and link checks. Sanitizer compiler exclusions are restricted to nginx core's unaligned wire-format operations and zero-length string copies; the compression module is fully instrumented.

## License

BSD-2-Clause. See [LICENSE](LICENSE) for the original copyright notices.
