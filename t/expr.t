#!/usr/bin/perl

use strict;
use warnings;

use Test::More;
use Test::Nginx;
use FindBin;
use lib "$FindBin::Bin/lib";
use Compression;

plan skip_all => 'requires all compression backends' unless all_backends();

plan skip_all => 'expr is not enabled' unless $ENV{TEST_NGINX_EXPR};

my $t = Test::Nginx->new()->has(qw/http/);
my $plain = 'conditional compression ' x 1024;
$t->write_file('body.txt', $plain);
write_config($t, <<'EOF');
%%TEST_GLOBALS%%
daemon off;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    default_type text/plain;
    compression_types *;
    compression_http_version 1.0;
    compression_min_length 0;
    compression_proxied any;
    gzip off;
    server {
        listen 127.0.0.1:8080;
        root %%TESTDIR%%;
        expr enabled str_eq $arg_on 1;
        expr brotli str_eq $arg_br 1;
        expr zstd str_eq $arg_zstd 1;
        expr ignore str_eq $arg_ignore 1;
        expr short str_eq $arg_short 1;
        when enabled {
            compression on;
        }
        compression off;
        when brotli {
            compression_encodings br gzip;
            compression_brotli_level 0;
        }
        when zstd {
            compression_encodings zstd;
            compression_zstd_level -1;
        }
        compression_encodings gzip deflate;
        when ignore {
            compression_ignore_client_qvalue on;
        }
        when short {
            compression_max_length 1;
        }
        location / {}
        location /child/ {
            alias %%TESTDIR%%/;
            compression_encodings deflate;
        }
        location /redirect {
            rewrite ^ /body.txt?on=1&br=1 last;
        }
    }
}
EOF

$t->run();

check_response($t, request('/body.txt', 'gzip'), undef, $plain, 'default off');
check_response($t, request('/body.txt?on=1', 'gzip'), 'gzip', $plain, 'when on');
check_response($t, request('/body.txt?on=1&br=1', 'br, gzip'),
    'br', $plain, 'conditional encoding list');
check_response($t, request('/body.txt?on=1&zstd=1', 'zstd'),
    'zstd', $plain, 'conditional negative level');
check_response($t, request('/body.txt?on=1&br=1', 'br;q=0.5,gzip;q=1'),
    'gzip', $plain, 'conditional list respects weight');
check_response($t,
    request('/body.txt?on=1&br=1&ignore=1', 'br;q=0.5,gzip;q=1'),
    'br', $plain, 'conditional ignore weights');
check_response($t, request('/body.txt?on=1&short=1', 'gzip'),
    undef, $plain, 'conditional maximum');
check_response($t, request('/child/body.txt?on=1', 'gzip, deflate'),
    'deflate', $plain, 'child replaces whole list');
check_response($t, request('/redirect', 'gzip, br'),
    'br', $plain, 'redirect evaluates destination arguments');

$t->stop();
unlike($t->read_file('error.log'), qr/\[(?:alert|emerg)\]|Sanitizer|runtime error:/,
    'no lifecycle or sanitizer errors');
undef $t;
done_testing();
