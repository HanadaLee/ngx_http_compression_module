#!/usr/bin/perl

use strict;
use warnings;

use Test::More;
use Test::Nginx;
use FindBin;
use lib "$FindBin::Bin/lib";
use Compression;

my @available = encodings();
my %available = map { $_ => 1 } @available;
my $default = $available{gzip} ? 'gzip' : $available{br} ? 'br' : 'zstd';
my $t = Test::Nginx->new()->has(qw/http/);
my @directives = (
    ['gzip', 'compression_gzip_level 1;'],
    ['gzip', 'compression_gzip_window 32k;'],
    ['gzip', 'compression_gzip_hash 64k;'],
    ['deflate', 'compression_deflate_level 1;'],
    ['deflate', 'compression_deflate_window 32k;'],
    ['deflate', 'compression_deflate_hash 64k;'],
    ['br', 'compression_brotli_level 6;'],
    ['br', 'compression_brotli_window 512k;'],
    ['zstd', 'compression_zstd_level 1;'],
    ['zstd', 'compression_zstd_window 512k;'],
);

for my $encoding (qw/gzip br zstd deflate/) {
    my ($ok, $log) = config_test("compression_encodings $encoding;");
    is($ok, $available{$encoding} ? 1 : 0, "$encoding configuration availability");
    if (!$available{$encoding}) {
        like($log, qr/encoding "\Q$encoding\E" is not available/,
            "$encoding reports unavailable backend");
    }
}
for my $case (@directives) {
    my ($encoding, $directive) = @$case;
    my ($ok, $log) = config_test($directive);
    is($ok, $available{$encoding} ? 1 : 0, "$directive availability");
    if (!$available{$encoding}) {
        my ($name) = split / /, $directive;
        like($log, qr/unknown directive "\Q$name\E"/,
            "$name is not registered");
    }
}
if (defined $ENV{TEST_NGINX_GZIP} && !$ENV{TEST_NGINX_GZIP}) {
    for my $directive (qw/gzip gzip_static gunzip/) {
        my ($ok, $log) = config_test("$directive on;");
        is($ok, 0, "$directive module is absent");
        like($log, qr/unknown directive "\Q$directive\E"/,
            "$directive was not enabled by core support");
    }
}

undef $t;
$t = Test::Nginx->new()->has(qw/http/);

my $plain = 'optional compression backend ' x 1024;
$t->write_file('body.txt', $plain);
my $conditional = $ENV{TEST_NGINX_EXPR} ? <<'EOF' : '';
        expr skip str_eq $arg_skip 1;
        when skip {
            compression off;
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
    compression on;
    compression_types *;
    compression_min_length 0;
    compression_http_version 1.0;
    compression_proxied any;
    server {
        listen 127.0.0.1:8080;
        root %%TESTDIR%%;
__CONDITIONAL__
        location / {
            compression_encodings __ENCODINGS__;
        }
        location /default/ {
            alias %%TESTDIR%%/;
        }
        location /agent/ {
            alias %%TESTDIR%%/;
            compression_encodings __ENCODINGS__;
            compression_disable Blocked;
        }
    }
}
EOF
$config =~ s/__CONDITIONAL__/$conditional/;
write_config($t, $config);
$t->run();

check_response($t, request('/default/body.txt', join ', ', @available),
    $default, $plain, 'available default encoding');
for my $encoding (@available) {
    check_response($t, request('/body.txt', $encoding), $encoding, $plain,
        "$encoding compressed response");
    like(request('/body.txt', $encoding), qr/^Vary: Accept-Encoding\r?$/mi,
        "$encoding core Vary support");
    check_response($t, request('/agent/body.txt', $encoding,
        "User-Agent: Blocked\r\n"), undef, $plain, "$encoding core disable");
    if ($ENV{TEST_NGINX_EXPR}) {
        check_response($t, request('/body.txt?skip=1', $encoding), undef, $plain,
            "$encoding conditional disable");
    }
}
for my $encoding (grep { !$available{$_} } qw/gzip br zstd deflate/) {
    check_response($t, request('/body.txt', $encoding), undef, $plain,
        "$encoding request falls back to identity");
    like(request('/body.txt', "$encoding, identity;q=0"), qr/^HTTP\/1\.1 406 /,
        "$encoding cannot bypass identity exclusion");
}

$t->stop();
unlike($t->read_file('error.log'), qr/\[(?:alert|emerg)\]|Sanitizer|runtime error:/,
    'no backend lifecycle errors');
undef $t;
done_testing();

sub config_test {
    my ($directive) = @_;
    $t->write_file_expand('nginx.conf', <<EOF);
%%TEST_GLOBALS%%
error_log %%TESTDIR%%/config.log;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    $directive
    server { listen 127.0.0.1:8080; }
}
EOF
    my $pid = fork();
    die "fork: $!" unless defined $pid;
    if (!$pid) {
        open STDOUT, '>', $t->testdir() . '/command.log' or die $!;
        open STDERR, '>&', \*STDOUT or die $!;
        exec $ENV{TEST_NGINX_BINARY}, '-t', '-e', $t->testdir() . '/config.log',
            '-p', $t->testdir(), '-c', 'nginx.conf';
        die "exec: $!";
    }
    waitpid($pid, 0);
    return ($? == 0 ? 1 : 0, $t->read_file('command.log'));
}
