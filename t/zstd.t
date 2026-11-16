#!/usr/bin/perl

use strict;
use warnings;

use Test::More;
use Test::Nginx qw/:DEFAULT http_content/;
use JSON::PP;
use FindBin;
use lib "$FindBin::Bin/lib";
use Compression;

plan skip_all => 'requires zstd' unless grep { $_ eq 'zstd' } encodings();

my $t = Test::Nginx->new()->has(qw/http/);
my $plain = join '', map { sprintf "%08x zstd window %c\n", $_, $_ % 251 }
    (0 .. 65535);
$t->write_file('body.txt', $plain);
my $config = <<'EOF';
%%TEST_GLOBALS%%
daemon off;
error_log %%TESTDIR%%/error.log debug;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    default_type text/plain;
    gzip off;
    compression on;
    compression_types *;
    compression_min_length 0;
    compression_encodings zstd;
    compression_buffers 2 256;
    output_buffers 1 8k;
    server {
        listen 127.0.0.1:8080;
        root %%TESTDIR%%;
        location / {}
        location /small/ {
            alias %%TESTDIR%%/;
            compression_zstd_window 1k;
        }
        location /wide/ {
            alias %%TESTDIR%%/;
            compression_zstd_window 8m;
        }
        location /negative/ {
            alias %%TESTDIR%%/;
            compression_zstd_level -1;
        }
        location /high/ {
            alias %%TESTDIR%%/;
            compression_zstd_window 8m;
            compression_zstd_level 20;
        }
    }
}
EOF
write_config($t, $config);
$t->run();

my @requests = (
    ['/', 512 * 1024],
    ['/small/', 1024],
    ['/wide/', 8 * 1024 * 1024],
    ['/negative/', 512 * 1024],
    ['/high/', 8 * 1024 * 1024],
    ['/', 512 * 1024],
);

for my $case (@requests) {
    my ($path, $window) = @$case;
    my $response = request($path . 'body.txt', 'zstd');
    check_response($t, $response, 'zstd', $plain, "zstd $path");
    $t->write_file('frame.zst', http_content($response));
    open my $probe, '-|', 'python3', '-B', "$FindBin::Bin/lib/zstd-frame.py",
        $t->testdir() . '/frame.zst' or die $!;
    my $json = do { local $/; <$probe> };
    close $probe;
    is($?, 0, 'libzstd accepts frame header');
    my $frame = decode_json($json);
    cmp_ok($frame->{window_size}, '<=', $window, "$path window limit");
}

$t->stop();
my $log = $t->read_file('error.log');
unlike($log, qr/\[(?:alert|emerg)\]|Sanitizer|runtime error:/,
    'no zstd lifecycle errors');
undef $t;
done_testing();
