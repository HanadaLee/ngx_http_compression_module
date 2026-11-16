#!/usr/bin/perl

use strict;
use warnings;

use Test::More;
use Test::Nginx;
use FindBin;
use lib "$FindBin::Bin/lib";
use Compression;

my $t = Test::Nginx->new()->has(qw/http rewrite proxy/);
my $plain = 'normalized compression response ' x 1024;
$t->write_file('body.txt', $plain);
is(system('python3', '-B', "$FindBin::Bin/lib/encode.py", 'deflate',
    $t->testdir() . '/body.txt', $t->testdir() . '/body.txt.deflate'),
    0, 'static fallback fixture');

my $conditional = $ENV{TEST_NGINX_EXPR} ? <<'EOF' : '';
        expr br_profile str_eq $arg_profile br;
        expr disabled_profile str_eq $arg_profile off;
        when br_profile {
            compression_normalize_accept_encoding br;
        }
        when disabled_profile {
            compression_normalize_accept_encoding off;
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
    compression off;
    compression_vary on;
    compression_min_length 0;
    compression_types text/plain;
    compression_encodings gzip deflate;
    server {
        listen 127.0.0.1:8080;
        root %%TESTDIR%%;
__CONDITIONAL__
        compression_normalize_accept_encoding gzip,br gzip;
        add_header X-Normalized $http_accept_encoding always;
        add_header X-Original $compression_original_accept_encoding always;
        location / {}
        location /off/ {
            alias %%TESTDIR%%/;
            compression_normalize_accept_encoding off;
        }
        location /compressed/ {
            alias %%TESTDIR%%/;
            compression on;
            compression_normalize_accept_encoding gzip deflate;
        }
        location /stored/ {
            alias %%TESTDIR%%/;
            compression_static on;
            compression_static_encodings gzip deflate;
            compression_normalize_accept_encoding gzip,deflate gzip deflate;
        }
        location /buffered/ {
            proxy_pass http://127.0.0.1:8081/;
        }
        location /unbuffered/ {
            proxy_buffering off;
            proxy_pass http://127.0.0.1:8081/;
        }
        location /upstream-only/ {
            compression_normalize_accept_encoding custom-coding br gzip;
            proxy_pass http://127.0.0.1:8081/;
        }
    }
    server {
        listen 127.0.0.1:8081;
        root %%TESTDIR%%;
        add_header X-Upstream-Encoding $http_accept_encoding always;
        add_header X-Upstream-Kept $http_x_kept always;
        location / {}
    }
}
EOF
$config =~ s/__CONDITIONAL__/$conditional/;
write_config($t, $config);
$t->run();

my $response = request('/body.txt', 'BR, GZip');
check_response($t, $response, undef, $plain, 'normalization without compression');
check_headers($response, 'gzip,br', 'BR, GZip', 'case and combination order');

for my $accept ('gzip;q=0, br;q=1', 'deflate') {
    $response = request('/body.txt', $accept);
    check_headers($response, undef, $accept, "$accept unmatched");
}

$response = request('/body.txt', undef);
check_headers($response, undef, undef, 'absent header');
$response = request('/off/body.txt', 'br, gzip');
check_headers($response, 'br, gzip', 'br, gzip', 'off overrides inheritance');

if ($ENV{TEST_NGINX_EXPR}) {
    $response = request('/body.txt?profile=br', 'gzip, br');
    check_headers($response, 'br', 'gzip, br', 'conditional combination');
    $response = request('/body.txt?profile=other', 'br, gzip');
    check_headers($response, 'gzip,br', 'br, gzip', 'conditional fallback');
    $response = request('/body.txt?profile=off', 'br, gzip');
    check_headers($response, 'br, gzip', 'br, gzip', 'conditional off');
}

for my $case (['deflate;q=1, gzip;q=0.1', 'gzip'],
              ['gzip;q=0, deflate;q=1', 'deflate'])
{
    my ($accept, $encoding) = @$case;
    $response = request('/compressed/body.txt', $accept);
    check_response($t, $response, $encoding, $plain, "$encoding after normalization");
    check_headers($response, $encoding, $accept, "$encoding after normalization");
    like($response, qr/^Vary: Accept-Encoding\r?$/mi,
        "$encoding normalization preserves Vary");
}

$response = request('/stored/body.txt', 'deflate;q=1, gzip;q=0.2');
check_response($t, $response, 'deflate', $plain, 'normalized static fallback');
check_headers($response, 'gzip,deflate', 'deflate;q=1, gzip;q=0.2',
    'static receives the complete normalized combination');
like($response, qr/^Vary: Accept-Encoding\r?$/mi, 'static normalization preserves Vary');

for my $prefix ('buffered', 'unbuffered') {
    $response = request("/$prefix/body.txt", 'BR, gzip;q=0.1');
    check_response($t, $response, undef, $plain, "$prefix proxy body");
    check_headers($response, 'gzip,br', 'BR, gzip;q=0.1', "$prefix proxy");
    like($response, qr/^X-Upstream-Encoding: gzip,br\r?$/mi,
        "$prefix proxy sends normalized header");
    $response = request("/$prefix/body.txt", 'deflate');
    unlike($response, qr/^X-Upstream-Encoding:/mi,
        "$prefix proxy does not forward a removed header");
}

$response = request('/upstream-only/body.txt', 'custom-coding');
check_headers($response, 'custom-coding', 'custom-coding', 'upstream-only encoding');
like($response, qr/^X-Upstream-Encoding: custom-coding\r?$/mi,
    'normalization does not require a local compression library');

for my $position (0, 1, 10, 19, 20, 21, 39, 40, 42) {
    my @headers = ('Host: localhost', 'X-Kept: retained',
        map { "X-Padding-$_: padding" } 1 .. 40);
    splice @headers, $position, 0, 'Accept-Encoding: deflate';
    $response = http("GET /buffered/body.txt HTTP/1.0\r\n"
        . join("\r\n", @headers) . "\r\n\r\n");
    like($response, qr/^HTTP\/1.1 200 /, "header removal at position $position");
    unlike($response, qr/^X-Upstream-Encoding:/mi,
        "removed header is not forwarded at position $position");
    like($response, qr/^X-Upstream-Kept: retained\r?$/mi,
        "other headers survive removal at position $position");
}

$response = http("GET /buffered/body.txt HTTP/1.0\r\n"
    . "Accept-Encoding: deflate\r\n\r\n");
like($response, qr/^HTTP\/1.1 200 /, 'removing the only request header');
unlike($response, qr/^X-Upstream-Encoding:/mi, 'the only header is not forwarded');

$response = request('/buffered/body.txt', 'deflate',
    "Accept-Encoding: deflate\r\nX-Kept: retained\r\n");
unlike($response, qr/^X-Upstream-Encoding:/mi, 'all rejected header fields are removed');
like($response, qr/^X-Upstream-Kept: retained\r?$/mi,
    'other headers survive repeated field removal');

$t->stop();
unlike($t->read_file('error.log'), qr/\[(?:alert|emerg)\]|Sanitizer|runtime error:/,
    'no normalization lifecycle errors');
undef $t;
done_testing();

sub check_headers {
    my ($response, $normalized, $original, $name) = @_;
    for my $field (['X-Normalized', $normalized], ['X-Original', $original]) {
        my ($header, $value) = @$field;
        if (defined $value) {
            like($response, qr/^\Q$header\E: \Q$value\E\r?$/mi, "$name $header");
        } else {
            unlike($response, qr/^\Q$header\E:/mi, "$name $header absent");
        }
    }
}
