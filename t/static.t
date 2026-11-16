#!/usr/bin/perl

use strict;
use warnings;

use Test::More;
use Test::Nginx qw/http http_content/;
use FindBin;
use lib "$FindBin::Bin/lib";
use Compression;

my $t = Test::Nginx->new()->has(qw/http/);
my $plain = 'static compression replacement ' x 4096;
my %suffix = (gzip => 'gz', br => 'br', zstd => 'zst', deflate => 'deflate');
my @encodings = encodings();
$t->write_file('body.txt', $plain);
mkdir $t->testdir() . '/only';
mkdir $t->testdir() . '/fallback';
mkdir $t->testdir() . '/suffixes';
$t->write_file('fallback/body.txt', $plain);
$t->write_file('suffixes/plain.txt', $plain);
for my $encoding (@encodings) {
    my $path = $t->testdir() . '/body.txt.' . $suffix{$encoding};
    is(system('python3', '-B', "$FindBin::Bin/lib/encode.py", $encoding,
        $t->testdir() . '/body.txt', $path), 0, "$encoding fixture");
    $t->write_file('only/body.txt.' . $suffix{$encoding},
        $t->read_file('body.txt.' . $suffix{$encoding}));
    $t->write_file("suffixes/$encoding.txt", $plain);
    $t->write_file("suffixes/$encoding.txt." . $suffix{$encoding},
        $t->read_file('body.txt.' . $suffix{$encoding}));
    $t->write_file("only/$encoding.txt." . $suffix{$encoding},
        $t->read_file('body.txt.' . $suffix{$encoding}));
}
$t->write_file('fallback/body.txt.gz', $t->read_file('body.txt.gz'));

my $conditional = $ENV{TEST_NGINX_EXPR} ? <<'EOF' : '';
    expr plain str_eq $arg_plain 1;
    when plain {
        compression_static off;
    }
EOF
my $config = <<'EOF';
%%TEST_GLOBALS%%
daemon off;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    default_type text/plain;
    gzip off;
    gzip_vary on;
    sendfile on;
    open_file_cache max=100;
__CONDITIONAL__
    compression_static on;
    compression_static_encodings __ENCODINGS__;
    server {
        listen 127.0.0.1:8080;
        root %%TESTDIR%%;
        location / {}
        location /alias/ {
            alias %%TESTDIR%%/;
        }
        location /always/ {
            alias %%TESTDIR%%/only/;
            compression_static always;
            compression_static_encodings gzip;
            decompression on;
        }
        location /order/ {
            alias %%TESTDIR%%/;
            compression_static_ignore_client_qvalue on;
            compression_static_encodings gzip deflate;
        }
        location /suffix-order/ {
            alias %%TESTDIR%%/suffixes/;
            compression_static_ignore_client_qvalue on;
        }
        location /suffix-always/ {
            alias %%TESTDIR%%/only/;
            compression_static always;
        }
    }
}
EOF
$config =~ s/__CONDITIONAL__/$conditional/;
write_config($t, $config);
$t->run();

for my $encoding (@encodings) {
    check_response($t, request('/body.txt', $encoding), $encoding, $plain,
        "$encoding static file");
    check_response($t, request('/alias/body.txt', $encoding), $encoding, $plain,
        "$encoding alias");
    my $response = request('/body.txt', "$encoding;q=0, identity;q=1");
    check_response($t, $response, undef, $plain, "$encoding q=0");
    like($response, qr/^Vary: Accept-Encoding\r?$/mi, "$encoding identity varies");
    check_response($t, request('/body.txt', "$encoding;q=0.2, identity;q=1"),
        undef, $plain, "$encoding identity preference");
    $response = request('/body.txt', $encoding, "Range: bytes=0-9\r\n");
    like($response, qr/^HTTP\/1.1 206 /, "$encoding range status");
    is(http_content($response), substr($t->read_file('body.txt.' .
        $suffix{$encoding}), 0, 10), "$encoding range uses encoded offsets");
}
check_response($t, request('/fallback/body.txt', join(', ',
    map { "$_;q=1" } grep { $_ ne 'gzip' } @encodings) . ', gzip;q=0.5'),
    'gzip', $plain, 'missing preferred file falls back to gzip');

for my $encoding (@encodings) {
    my $accept = join(', ', map { "$_;q=1" }
        grep { $_ ne $encoding } @encodings) . ", $encoding;q=0.1";
    check_response($t, request("/suffixes/$encoding.txt", $accept),
        $encoding, $plain, "$encoding after all preferred files are missing");
    check_response($t, request("/only/$encoding.txt", $accept),
        $encoding, $plain, "$encoding fallback without an uncompressed file");
    check_response($t, request("/suffix-order/$encoding.txt", $accept),
        $encoding, $plain, "$encoding fallback in configured order");
    check_response($t, request("/suffix-always/$encoding.txt", 'identity'),
        $encoding, $plain, "$encoding always fallback");
    check_response($t, request("/suffixes/$encoding.txt",
        "*;q=1, $encoding;q=0"), undef, $plain,
        "$encoding q=0 is not a fallback");
}

for my $path ('/suffixes/plain.txt', '/suffix-order/plain.txt') {
    check_response($t, request($path, join(', ', @encodings)), undef, $plain,
        "$path falls back to the uncompressed file");
}
for my $path ('/suffixes/missing.txt', '/suffix-order/missing.txt',
    '/suffix-always/missing.txt')
{
    like(request($path, join(', ', @encodings)), qr/^HTTP\/1.1 404 /,
        "$path finishes when all files are missing");
}

check_response($t, request('/always/body.txt', 'identity'), undef, $plain,
    'compressed-only file is decompressed');
check_response($t, request('/always/body.txt', 'gzip'), 'gzip', $plain,
    'compressed-only file is sent directly');
check_response($t, request('/order/body.txt', 'deflate;q=1, gzip;q=0.1'),
    'gzip', $plain, 'static server preference');

my $response = request('/body.txt', 'gzip');
my ($etag) = $response =~ /^ETag: (.+)\r$/m;
ok($etag, 'static ETag');
like(request('/body.txt', 'gzip', "If-None-Match: $etag\r\n"),
    qr/^HTTP\/1.1 304 /, 'static conditional response');
$response = http("HEAD /body.txt HTTP/1.1\r\nHost: localhost\r\n"
    . "Accept-Encoding: gzip\r\nConnection: close\r\n\r\n");
like($response, qr/^Content-Encoding: gzip\r?$/mi, 'HEAD encoding');
is(http_content($response), '', 'HEAD has no body');
if ($ENV{TEST_NGINX_EXPR}) {
    check_response($t, request('/body.txt?plain=1', 'gzip'), undef, $plain,
        'conditional static disable');
}

$t->stop();
unlike($t->read_file('error.log'), qr/\[(?:alert|emerg)\]|Sanitizer|runtime error:/,
    'no static lifecycle errors');
undef $t;
done_testing();
