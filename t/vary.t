#!/usr/bin/perl

use strict;
use warnings;

use Test::More;
use Test::Nginx;
use FindBin;
use lib "$FindBin::Bin/lib";
use Compression;

my $t = Test::Nginx->new()->has(qw/http proxy/);
my $plain = 'compression vary response ' x 1024;
$t->write_file('body.txt', $plain);
prepare_static($t, $plain);

my $native = !defined $ENV{TEST_NGINX_GZIP} || $ENV{TEST_NGINX_GZIP} ? <<'EOF' : '';
        location /native {
            compression off;
            gzip on;
            gzip_min_length 0;
            gzip_types text/plain;
            return 200 native-compression;
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
    gzip_vary off;
    compression_vary on;
    compression on;
    compression_types *;
    compression_min_length 0;
    compression_encodings __ENCODINGS__;
    server {
        listen 127.0.0.1:8080;
        root %%TESTDIR%%;
        location / {}
        location /merge {
            add_header Vary "Origin, accept-encoding";
            add_header Vary "User-Agent, Origin";
            return 200 merge-response;
        }
        location /existing {
            compression off;
            add_header Vary "Origin, User-Agent";
            add_header Vary origin;
            return 200 existing-response;
        }
        location /off {
            compression_vary off;
            return 200 off-response;
        }
        location /plain {
            compression off;
            return 200 plain-response;
        }
        location /stored/ {
            alias %%TESTDIR%%/precompressed/;
            compression off;
            compression_static on;
            compression_static_encodings __ENCODINGS__;
            add_header Vary Origin;
        }
        location /decoded/ {
            compression off;
            decompression on;
            proxy_pass http://127.0.0.1:8081/compressed/;
        }
        location /buffered/ {
            proxy_pass http://127.0.0.1:8081/source/;
        }
        location /unbuffered/ {
            proxy_buffering off;
            proxy_pass http://127.0.0.1:8081/source/;
        }
__NATIVE__
    }
    server {
        listen 127.0.0.1:8081;
        root %%TESTDIR%%;
        compression off;
        compression_vary off;
        add_header Vary Origin;
        add_header Vary "X-Example, origin";
        location /source/ {
            alias %%TESTDIR%%/;
        }
        location /compressed/ {
            alias %%TESTDIR%%/precompressed/;
            compression_static always;
            compression_static_encodings __ENCODINGS__;
        }
    }
}
EOF
$config =~ s/__NATIVE__/$native/;
write_config($t, $config);
$t->run();

for my $encoding (encodings()) {
    my $response = request('/body.txt', $encoding);
    check_response($t, $response, $encoding, $plain, "$encoding Vary filter");
    check_vary($response, 'Accept-Encoding', "$encoding Vary filter");

    $response = request("/stored/$encoding.txt", $encoding);
    check_response($t, $response, $encoding, $plain, "$encoding static Vary");
    check_vary($response, 'Origin, Accept-Encoding', "$encoding static Vary");

    for my $path ('/buffered/body.txt', '/unbuffered/body.txt') {
        $response = request($path, $encoding);
        check_response($t, $response, $encoding, $plain, "$encoding $path");
        check_vary($response, 'Origin, X-Example, Accept-Encoding',
            "$encoding $path upstream Vary");
    }
}

for my $encoding (decodings()) {
    next unless grep { $_ eq $encoding } encodings();
    my $response = request("/decoded/$encoding.txt", 'identity');
    check_response($t, $response, undef, $plain, "$encoding decompressed Vary");
    check_vary($response, 'Origin, X-Example, Accept-Encoding',
        "$encoding decompressed Vary");
}

check_vary(request('/body.txt', undef), 'Accept-Encoding',
    'compressible response without Accept-Encoding');
check_vary(request('/merge', 'gzip'), 'Origin, accept-encoding, User-Agent',
    'late response headers are merged and deduplicated');
check_vary(request('/existing', 'identity'), 'Origin, User-Agent',
    'existing Vary is deduplicated without compression');
unlike(request('/off', 'gzip'), qr/^Vary:/mi, 'off overrides inherited setting');
unlike(request('/plain', 'identity'), qr/^Vary:/mi, 'plain response stays unchanged');

my $response = request('/stored/gzip.txt', 'gzip', "Range: bytes=0-9\r\n");
like($response, qr/^HTTP\/1.1 206 /, 'encoded range status');
check_vary($response, 'Origin, Accept-Encoding', 'Vary after range filtering');

if (length $native) {
    $response = request('/native', 'gzip');
    check_response($t, $response, 'gzip', 'native-compression', 'native gzip Vary');
    check_vary($response, 'Accept-Encoding', 'native gzip Vary');
}

$t->stop();
unlike($t->read_file('error.log'), qr/\[(?:alert|emerg)\]|Sanitizer|runtime error:/,
    'no Vary lifecycle errors');
undef $t;
done_testing();

sub check_vary {
    my ($response, $expected, $name) = @_;
    my @fields = $response =~ /^Vary:\s*(.*?)\r?$/gmi;
    is(scalar @fields, 1, "$name has one field");
    is($fields[0], $expected, "$name value");
}
