#!/usr/bin/perl

use strict;
use warnings;

use Test::More;
use Test::Nginx qw/:DEFAULT http_content/;
use IO::Compress::Gzip qw/gzip/;
use FindBin;
use lib "$FindBin::Bin/lib";
use Compression;

plan skip_all => 'requires all compression backends' unless all_backends();

my $t = Test::Nginx->new()->has(qw/http proxy cache slice sub gzip_static gunzip/);
my $plain = 'hello filter chain ' x 4096;
$t->write_file('body.txt', $plain);
my $compressed;
gzip \$plain => \$compressed or die "gzip fixture";
$t->write_file('body.txt.gz', $compressed);
my $config = <<'EOF';
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
    compression_buffers 2 512;
    proxy_cache_path %%TESTDIR%%/cache keys_zone=cache:1m;
    server {
        listen 127.0.0.1:8080;
        root %%TESTDIR%%;
        location /static/ {
            alias %%TESTDIR%%/;
            gzip_static on;
        }
        location /sub/ {
            alias %%TESTDIR%%/;
            sub_filter_types *;
            sub_filter hello HELLO_LONGER;
            sub_filter_once off;
        }
        location /cache/ {
            proxy_pass http://127.0.0.1:8081/;
            proxy_set_header Accept-Encoding "";
            proxy_cache cache;
            proxy_cache_valid 1m;
            add_header X-Cache $upstream_cache_status;
        }
        location /slice/ {
            slice 4k;
            proxy_pass http://127.0.0.1:8081/;
            proxy_set_header Range $slice_range;
            proxy_set_header Accept-Encoding "";
            proxy_cache cache;
            proxy_cache_key "$uri$slice_range";
            proxy_cache_valid 200 206 1m;
        }
        location /unzip/ {
            gunzip on;
            proxy_pass http://127.0.0.1:8081/gzip/;
        }
        location /native-off/ {
            gzip_proxied off;
            proxy_pass http://127.0.0.1:8081/;
        }
        location /alias-off/ {
            gzip_proxied any;
            compression_proxied off;
            proxy_pass http://127.0.0.1:8081/;
        }
        location /agents/ {
            alias %%TESTDIR%%/;
            gzip_disable FirstAgent;
            compression_disable SecondAgent;
        }
        location /notfound {
            return 404 'hello error body';
        }
        location /forbidden {
            return 403 'hello forbidden body';
        }
        location /nobody {
            return 204;
        }
        location / {}
        location /normalize/ {
            compression_normalize_accept_encoding br gzip;
            alias %%TESTDIR%%/;
        }
        location /vary/ {
            gzip_vary off;
            compression_vary on;
            proxy_pass http://127.0.0.1:8081/vary/;
        }
    }
    server {
        listen 127.0.0.1:8081;
        root %%TESTDIR%%;
        compression off;
        location /gzip/ {
            alias %%TESTDIR%%/;
            gzip_static always;
        }
        location /vary/ {
            alias %%TESTDIR%%/;
            add_header Vary Origin;
            add_header Vary X-Example;
        }
        location / {}
    }
}
EOF
$t->write_file_expand('nginx.conf', $config);
$t->run();

for my $encoding (qw/gzip br zstd deflate/) {
    my $changed = $plain;
    $changed =~ s/hello/HELLO_LONGER/g;
    check_response($t, request('/sub/body.txt', $encoding), $encoding, $changed,
        "$encoding after substitution");
    check_response($t, request('/cache/body.txt', $encoding), $encoding, $plain,
        "$encoding cached response");
    my $cached = request('/cache/body.txt', $encoding);
    like($cached, qr/^X-Cache: HIT\r?$/mi, "$encoding cache hit");
    check_response($t, $cached, $encoding, $plain, "$encoding cache hit body");
    check_response($t, request('/slice/body.txt', $encoding), $encoding, $plain,
        "$encoding slice response");
    check_response($t, request('/agents/body.txt', $encoding,
        "User-Agent: FirstAgent\r\n"), undef, $plain, "$encoding native disable");
    check_response($t, request('/agents/body.txt', $encoding,
        "User-Agent: SecondAgent\r\n"), undef, $plain, "$encoding alias disable");
    for my $status (['notfound', 404, 'hello error body'],
                    ['forbidden', 403, 'hello forbidden body']) {
        my $response = request("/$status->[0]", $encoding);
        like($response, qr/^HTTP\/1\.1 $status->[1] /, "$encoding error status");
        like($response, qr/^Content-Encoding: \Q$encoding\E\r?$/mi,
            "$encoding error encoded");
        is(decoded($t, $response, $status->[2]), $status->[2],
            "$encoding error round trip");
    }
}
check_response($t, request('/static/body.txt', 'gzip'), 'gzip', $plain,
    'native precompressed file');
check_response($t, request('/unzip/body.txt', 'br'), 'br', $plain,
    'gunzip before new compressor');
check_response($t, request('/unzip/body.txt', 'gzip'), 'gzip', $plain,
    'upstream encoding passed through');
for my $prefix (qw/native-off alias-off/) {
    check_response($t, request("/$prefix/body.txt", 'br',
        "Via: 1.1 proxy\r\n"), undef, $plain, "$prefix core shared field");
}
my $single = request('/body.txt', 'identity', "Range: bytes=0-4\r\n");
like($single, qr/^HTTP\/1\.1 206 /, 'single range status');
is(http_content($single), 'hello', 'single range bytes');
my $multi = request('/body.txt', 'identity', "Range: bytes=0-4,6-11\r\n");
like($multi, qr/^HTTP\/1\.1 206 /, 'multiple range status');
like($multi, qr/multipart\/byteranges/i, 'multiple range type');
like($multi, qr/Content-Range: bytes 0-4\//, 'first range retained');
like($multi, qr/Content-Range: bytes 6-11\//, 'second range retained');

my $none = request('/nobody', 'gzip');
like($none, qr/^HTTP\/1\.1 204 /, '204 retained');
unlike($none, qr/^Content-Encoding:/mi, '204 not compressed');
my $head = http("HEAD /body.txt HTTP/1.0\r\nHost: localhost\r\n"
    . "Accept-Encoding: gzip\r\n\r\n");
unlike($head, qr/^Content-Encoding:/mi, 'HEAD not compressed');

check_response($t,
    request('/normalize/body.txt', 'gzip;q=1, br;q=0.1'),
    'br', $plain, 'normalize remains authoritative');
my $vary = request('/vary/body.txt', 'zstd');
check_response($t, $vary, 'zstd', $plain, 'independent Vary filter');
my @headers = $vary =~ /^Vary: (.*?)\r?$/gmi;
is(scalar @headers, 1, 'one merged Vary field');
like($headers[0], qr/Origin/i, 'original Vary retained');
like($headers[0], qr/X-Example/i, 'second original Vary retained');
like($headers[0], qr/Accept-Encoding/i, 'compression Vary appended');

$t->stop();
unlike($t->read_file('error.log'), qr/\[(?:alert|emerg)\]|Sanitizer|runtime error:/,
    'no lifecycle or sanitizer errors');
undef $t;
done_testing();
