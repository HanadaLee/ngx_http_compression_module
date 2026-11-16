#!/usr/bin/env bash
set -euo pipefail

module_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source_root=${TEST_NGINX_SOURCE:-$module_root/.ci/nginx}
harness_root=${TEST_NGINX_TESTS:-$module_root/.ci/nginx-tests}
output_root=$module_root/.ci/test-output
mkdir -p "$output_root"
export TMPDIR=$output_root/tmp
mkdir -p "$TMPDIR"

args=(
    --prefix="$output_root/prefix"
    --with-debug
    --with-http_ssl_module
    --with-http_v2_module
    --with-http_v3_module
    --with-http_sub_module
    --with-http_slice_module
    --with-http_degradation_module
)

if [[ ${TEST_NGINX_GZIP:-1} == 0 ]]; then
    args+=(--without-http_gzip_module)
else
    args+=(--with-http_gzip_static_module --with-http_gunzip_module)
fi

if [[ ${TEST_NGINX_EXPR:-0} == 1 ]]; then
    args+=(--add-module="${TEST_EXPR_SOURCE:-$module_root/.ci/ngx_expr_module}")
fi

if [[ -d "$module_root/.ci/normalize" && -d "$module_root/.ci/vary" ]]; then
    args+=(--add-module="$module_root/.ci/normalize"
           --add-module="$module_root/.ci/vary")
    export TEST_NGINX_COMPRESSION_AUX=1
fi

if [[ ${TEST_MODULE_LINK:-static} == dynamic ]]; then
    args+=(--with-compat --add-dynamic-module="$module_root")
else
    args+=(--add-module="$module_root")
fi

if [[ ${TEST_SANITIZE:-0} == 1 ]]; then
    args+=(
        --with-cc="sh $module_root/util/sanitizer-cc"
        "--with-cc-opt=-O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -fno-pie"
        "--with-ld-opt=-fsanitize=address,undefined -no-pie"
    )
    export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1
    export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
    export TEST_NGINX_GLOBALS="${TEST_NGINX_GLOBALS:-} env ASAN_OPTIONS; env UBSAN_OPTIONS;"
fi

if [[ -n ${TEST_COMPRESSION_LIBRARIES:-} ]]; then
    args+=(--with-cc="sh $module_root/util/library-cc")
fi

cd "$source_root"
configure_nginx() {
    if [[ ${TEST_NGINX_OPENRESTY:-0} == 1 ]]; then
        perl configure "${args[@]}"
    elif [[ -f auto/configure ]]; then
        sh auto/configure "${args[@]}"
    else
        sh configure "${args[@]}"
    fi
}
if configure_nginx 2>&1 | tee "$output_root/configure.log"; then
    configured=1
else
    configured=0
fi

if [[ -n ${TEST_COMPRESSION_LIBRARIES:-}
      && " $TEST_COMPRESSION_LIBRARIES " != *" zlib "* ]]; then
    [[ $configured == 0 ]]
    grep -q 'ngx_http_compression_module requires the zlib library' \
        "$output_root/configure.log"
    echo 'PASS: configure rejects a build without zlib'
    exit 0
fi
[[ $configured == 1 ]]
make -j"${TEST_BUILD_JOBS:-2}" 2>&1 | tee "$output_root/build.log"

export TEST_NGINX_BINARY="$source_root/objs/nginx"
auto_config=$source_root/objs/ngx_auto_config.h
if [[ ${TEST_NGINX_OPENRESTY:-0} == 1 ]]; then
    make install 2>&1 | tee "$output_root/install.log"
    export TEST_NGINX_BINARY="$output_root/prefix/nginx/sbin/nginx"
    for auto_config in "$source_root"/build/nginx-*/objs/ngx_auto_config.h; do
        [[ -f $auto_config ]] && break
    done
fi

encodings=()
if grep -Eq '^#define NGX_HTTP_COMPRESSION_HAVE_ZSTD[[:space:]]+1$' "$auto_config"; then
    encodings+=(zstd)
fi
if grep -Eq '^#define NGX_HTTP_COMPRESSION_HAVE_BROTLI[[:space:]]+1$' "$auto_config"; then
    encodings+=(br)
fi
encodings+=(gzip deflate)
export TEST_NGINX_COMPRESSION_ENCODINGS="${encodings[*]}"

decodings=(gzip deflate)
if grep -Eq '^#define NGX_HTTP_COMPRESSION_HAVE_BROTLI_DECODER[[:space:]]+1$' "$auto_config"; then
    decodings+=(br)
fi
if grep -Eq '^#define NGX_HTTP_COMPRESSION_HAVE_ZSTD_DECODER[[:space:]]+1$' "$auto_config"; then
    decodings+=(zstd)
fi
export TEST_NGINX_DECOMPRESSION_ENCODINGS="${decodings[*]}"

export TEST_NGINX_ZSTD_TARGET_BLOCK_SIZE=0
if grep -Eq '^#define NGX_HTTP_COMPRESSION_HAVE_ZSTD_TARGET_BLOCK_SIZE[[:space:]]+1$' "$auto_config"; then
    export TEST_NGINX_ZSTD_TARGET_BLOCK_SIZE=1
fi

export PERL5LIB="$harness_root/lib${PERL5LIB:+:$PERL5LIB}"
export TEST_NGINX_LEAVE=1
if [[ $(id -u) == 0 ]]; then
    export TEST_NGINX_GLOBALS="${TEST_NGINX_GLOBALS:-} user root;"
fi
if [[ ${TEST_MODULE_LINK:-static} == dynamic ]]; then
    export TEST_NGINX_MODULE_DIR="$source_root/objs"
    modules=(compression_filter compression_static )
    if [[ ${TEST_MODULE_ORDER:-forward} == reverse ]]; then
        modules=(compression_static compression_filter)
    fi
    for module in "${modules[@]}"; do
        export TEST_NGINX_GLOBALS="${TEST_NGINX_GLOBALS:-} load_module $source_root/objs/ngx_http_${module}_module.so;"
    done
fi
cd "$module_root"
prove -v t/*.t 2>&1 | tee "$output_root/tests.log"
