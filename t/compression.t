#!/usr/bin/perl

use strict;
use warnings;

use Test::More;
use Test::Nginx qw/:DEFAULT http_content/;
use FindBin;
use lib "$FindBin::Bin/lib";
use Compression;

plan skip_all => 'requires all compression backends' unless all_backends();

my $t = Test::Nginx->new()->has(qw/http/);
my $plain = join '', map { sprintf "%08x hello compression %c\n", $_, $_ % 251 }
    (0 .. 8191);
$t->write_file('body.txt', $plain);
$t->write_file('empty.txt', '');
$t->write_file('short.txt', 'small');
write_config($t, <<'EOF');
%%TEST_GLOBALS%%
daemon off;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    default_type text/plain;
    gzip off;
    gzip_vary on;
    compression on;
    compression_types *;
    compression_min_length 0;
    compression_http_version 1.0;
    compression_proxied any;
    compression_encodings zstd br gzip deflate;
    compression_buffers 2 128;
    log_format compression '$uri $sent_http_content_encoding $compression_ratio';
    access_log %%TESTDIR%%/compression.log compression;
    server {
        listen 127.0.0.1:8080;
        root %%TESTDIR%%;
        location / {}
        location /server/ {
            alias %%TESTDIR%%/;
            compression_ignore_client_qvalue on;
        }
        location /gzip/ {
            alias %%TESTDIR%%/;
            compression_encodings gzip;
        }
        location /off/ {
            alias %%TESTDIR%%/;
            compression off;
        }
        location /minimum/ {
            alias %%TESTDIR%%/;
            compression_min_length 10;
        }
        location /maximum/ {
            alias %%TESTDIR%%/;
            compression_max_length 100;
        }
        location /via/ {
            alias %%TESTDIR%%/;
            compression_proxied off;
        }
        location /agent/ {
            alias %%TESTDIR%%/;
            compression_disable Blocked;
        }
        location /http11/ {
            alias %%TESTDIR%%/;
            compression_http_version 1.1;
        }
        location /bypass/ {
            alias %%TESTDIR%%/;
            compression_bypass $arg_skip;
        }
        location /postpone/ {
            alias %%TESTDIR%%/;
            compression_postpone 1m;
        }
        location /nobuffer/ {
            alias %%TESTDIR%%/;
            compression_no_buffer on;
        }
        location /negative/ {
            alias %%TESTDIR%%/;
            compression_encodings zstd;
            compression_zstd_level -1;
        }
    }
}
EOF

$t->run();

check_response($t, request('/body.txt'), undef, $plain, 'missing accept');
check_response($t, request('/body.txt', ''), undef, $plain, 'empty accept');

for my $encoding (qw/gzip br zstd deflate/) {
    for my $prefix ('', '/postpone', '/nobuffer') {
        check_response($t, request("$prefix/body.txt", $encoding), $encoding,
            $plain, "$encoding $prefix");
    }
    for my $file ('empty.txt', 'short.txt') {
        my $body = $file eq 'empty.txt' ? '' : 'small';
        check_response($t, request("/$file", $encoding), $encoding, $body,
            "$encoding $file");
    }
}

my @negotiation = (
    ['gzip, br, zstd', 'zstd'],
    ['gzip;q=1, br;q=0.8, zstd;q=0.5', 'gzip'],
    ['zstd;q=0, br;q=0.5, gzip;q=0.5', 'br'],
    ['br;q=0, gzip', 'gzip'],
    ['br-invalid, gzip', 'gzip'],
    ['zstd;q=0, *;q=0.5', 'br'],
    ['identity;q=1, gzip;q=0.5', undef],
    ['gzip;q=0, gzip', undef],
    ['br;q=bad, br, gzip', 'gzip'],
    ['GZIP; Q=1.000', 'gzip'],
    [" \tbr ;q=0.001 \t", 'br'],
    ['gzip;q=1.001', undef],
    ['gzip;q=0.0001', undef],
    ['gzip;q=-1', undef],
    ['gzip;q=2', undef],
    ['gzip;q=0.', undef],
    ['gzip;q=1.', 'gzip'],
    ['unsupported', undef],
);
for my $case (@negotiation) {
    check_response($t, request('/body.txt', $case->[0]), $case->[1], $plain,
        "negotiate $case->[0]");
}
check_response($t,
    request('/server/body.txt', 'gzip;q=1, br;q=0.8, zstd;q=0.5'),
    'zstd', $plain, 'ignore client weights');
check_response($t, request('/server/body.txt', 'zstd;q=0, br;q=0.5, gzip;q=1'),
    'br', $plain, 'server priority preserves exclusions');
check_response($t, request('/body.txt', 'gzip;q=0.1',
    "Accept-Encoding: br;q=0.8\r\n"), 'br', $plain, 'multiple fields');

for my $accept ('identity;q=0', '*;q=0', 'gzip;q=0, identity;q=0') {
    like(request('/body.txt', $accept), qr/^HTTP\/1\.1 406 /,
        "unacceptable $accept");
}
check_response($t, request('/gzip/body.txt', 'br, gzip'), 'gzip', $plain,
    'child encoding list replaces parent');
check_response($t, request('/gzip/body.txt', 'br'), undef, $plain,
    'child does not inherit extra encodings');
check_response($t, request('/off/body.txt', 'gzip'), undef, $plain, 'off');
check_response($t, request('/minimum/short.txt', 'gzip'), undef, 'small',
    'minimum length');
check_response($t, request('/maximum/body.txt', 'gzip'), undef, $plain,
    'maximum length');
check_response($t, request('/via/body.txt', 'gzip', "Via: 1.1 proxy\r\n"),
    undef, $plain, 'core proxied off');
check_response($t, request('/via/body.txt', 'gzip'), 'gzip', $plain,
    'without Via');
check_response($t, request('/agent/body.txt', 'br', "User-Agent: Blocked\r\n"),
    undef, $plain, 'core agent filter applies to Brotli');
check_response($t, request('/http11/body.txt', 'gzip', '', '1.0'), undef,
    $plain, 'core protocol minimum');
check_response($t, request('/bypass/body.txt?skip=1', 'gzip'), undef, $plain,
    'variable bypass');
check_response($t, request('/bypass/body.txt?skip=0', 'gzip'), 'gzip', $plain,
    'zero bypass');
check_response($t, request('/negative/body.txt', 'zstd'), 'zstd', $plain,
    'negative one is a real Zstd level');

my $response = request('/body.txt', 'gzip');
unlike($response, qr/^Content-Length:/mi, 'compressed length cleared');
unlike($response, qr/^Accept-Ranges:/mi, 'range advertisement cleared');
like($response, qr/^ETag: W\//mi, 'compressed ETag is weak');
like($response, qr/^Vary: Accept-Encoding\r?$/mi, 'gzip_vary works');
like(request('/body.txt', 'identity'), qr/^Vary: Accept-Encoding\r?$/mi,
    'Vary retained for identity response');
$t->stop();
like($t->read_file('compression.log'), qr/body\.txt gzip \d+\.\d\d/,
    'completed ratio available in access log');
unlike($t->read_file('error.log'), qr/\[(?:alert|emerg)\]|Sanitizer|runtime error:/,
    'no encoder or lifecycle alerts');

undef $t;
done_testing();
