#!/usr/bin/perl

use strict;
use warnings;

use Test::More;
use Test::Nginx;

plan skip_all => 'dynamic modules only' unless $ENV{TEST_NGINX_MODULE_DIR};

my $t = Test::Nginx->new()->has(qw/http/);
my @cases = (
    ['compression_filter', 'compression on;'],
    ['compression_static', 'compression_static on;'],
    ['decompression_filter', 'decompression on;'],
);
for my $case (@cases) {
    my ($module, $directive) = @$case;
    $t->write_file_expand('standalone.conf', <<EOF);
load_module $ENV{TEST_NGINX_MODULE_DIR}/ngx_http_${module}_module.so;
error_log %%TESTDIR%%/standalone.log;
pid %%TESTDIR%%/standalone.pid;
events {}
http {
    access_log off;
    $directive
    server { listen 127.0.0.1:8080; }
}
EOF
    my $pid = fork();
    die "fork: $!" unless defined $pid;
    if (!$pid) {
        open STDOUT, '>', $t->testdir() . '/command.log' or die $!;
        open STDERR, '>&', \*STDOUT or die $!;
        exec $ENV{TEST_NGINX_BINARY}, '-t', '-p', $t->testdir(),
            '-c', 'standalone.conf';
        die "exec: $!";
    }
    waitpid($pid, 0);
    is($?, 0, "$module loads without the other two modules")
        or diag $t->read_file('command.log');
}
undef $t;
done_testing();
