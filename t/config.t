#!/usr/bin/perl

use strict;
use warnings;

use Test::More;
use Test::Nginx;
use FindBin;
use lib "$FindBin::Bin/lib";
use Compression;

plan skip_all => 'requires all compression backends' unless all_backends();

my $t = Test::Nginx->new()->has(qw/http/);
my @tests = (
    ['compression_encodings br gzip zstd deflate;', 1, 'all encodings'],
    ['compression_encodings gzip gzip;', 0, 'duplicate encoding'],
    ['compression_encodings unknown;', 0, 'unknown encoding'],
    ['compression_encodings identity;', 0, 'identity is not a compressor'],
    ['compression_ignore_client_qvalue maybe;', 0, 'invalid flag'],
    ['compression_gzip_level 0;', 0, 'gzip minimum'],
    ['compression_gzip_level 10;', 0, 'gzip maximum'],
    ['compression_brotli_level 0;', 1, 'Brotli zero'],
    ['compression_brotli_level 12;', 0, 'Brotli maximum'],
    ['compression_deflate_level -1;', 0, 'deflate negative'],
    ['compression_zstd_level -1;', 1, 'Zstd negative one'],
    ['compression_zstd_level 0;', 0, 'Zstd zero'],
    ['compression_zstd_window 1k;', 1, 'Zstd minimum window'],
    ['compression_zstd_window 8m;', 1, 'Zstd maximum HTTP window'],
    ['compression_zstd_window 16m;', 0, 'Zstd rejects oversized HTTP window'],
    ['compression_zstd_window 512;', 0, 'Zstd rejects undersized window'],
    ['compression_zstd_window 3k;', 0, 'Zstd rejects non-power-of-two window'],
    ['compression_zstd_level -1; compression_zstd_level -1;', 0,
        'negative one duplicate'],
    ['compression_gzip_window 512;', 1, 'minimum zlib window'],
    ['compression_gzip_window 1m;', 0, 'invalid zlib window'],
    ['compression_deflate_window 1000;', 0, 'non-power-of-two window'],
    ['compression_brotli_window 16m;', 1, 'Brotli maximum window'],
    ['compression_brotli_window 32m;', 0, 'invalid Brotli window'],
    ['compression_gzip_hash 128k;', 1, 'maximum hash'],
    ['compression_deflate_hash 256;', 0, 'invalid hash'],
    ['compression_buffers 0 4k;', 0, 'zero buffer count'],
    ['compression_buffers 1 0;', 0, 'zero buffer size'],
    ['compression_gzip_buffers 1 4k;', 0, 'no per-codec buffers'],
    ['compression_zstd_dict_file dict;', 0, 'no dictionary directive'],
    ['compression_http_version 2;', 0, 'invalid version alias'],
    ['gzip_http_version 1.0; compression_http_version 1.0;', 0,
        'same-value version alias is duplicate'],
    ['compression_http_version 1.0; gzip_http_version 1.1;', 0,
        'reverse version alias is duplicate'],
    ['gzip_proxied auth; compression_proxied no-cache;', 1,
        'proxied aliases accumulate'],
    ['compression_proxied invalid;', 0, 'invalid proxied alias'],
    ['gzip_disable First; compression_disable Second;', 1,
        'disable aliases accumulate'],
    ['compression_disable msie6;', 1, 'msie6 mask'],
    ['compression_disable "[";', 0, 'invalid alias regex'],
);
for my $test (@tests) {
    $t->write_file_expand('nginx.conf', <<EOF);
%%TEST_GLOBALS%%
error_log %%TESTDIR%%/config.log;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    $test->[0]
    server { listen 127.0.0.1:8080; }
}
EOF
    my $pid = fork();
    die "fork: $!" unless defined $pid;
    if (!$pid) {
        open STDOUT, '>', $t->testdir() . '/command.log' or die $!;
        open STDERR, '>&', \*STDOUT or die $!;
        exec $ENV{TEST_NGINX_BINARY}, '-t', '-e', $t->testdir() . '/config.log',
            '-p', $t->testdir(),
            '-c', 'nginx.conf';
        die "exec: $!";
    }
    waitpid($pid, 0);
    my $ok = $? == 0;
    is($ok ? 1 : 0, $test->[1], $test->[2])
        or diag($t->read_file('command.log'));
}
undef $t;
done_testing();
