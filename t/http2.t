#!/usr/bin/perl

use strict;
use warnings;

use Test::More;
use Test::Nginx;
use Test::Nginx::HTTP2;
use FindBin;
use lib "$FindBin::Bin/lib";
use Compression;

my $t = Test::Nginx->new()->has(qw/http http_v2 /);
my $plain = 'multiplexed compression ' x 1024;
$t->write_file('body.txt', $plain);

write_config($t, <<'EOF');
%%TEST_GLOBALS%%
daemon off;
events {}
http {
    %%TEST_GLOBALS_HTTP%%
    default_type text/plain;
    gzip off;
    compression on;
    compression_types *;
    compression_min_length 0;
    compression_encodings __ENCODINGS__;
    compression_buffers 1 64;
    server {
        listen 127.0.0.1:8080;
        http2 on;
        root %%TESTDIR%%;
        location / {}
    }
}
EOF
$t->run();

my $connection = Test::Nginx::HTTP2->new();
my @requests;
for my $encoding (encodings()) {
    my $sid = $connection->new_stream({ headers => [
        { name => ':method', value => 'GET', mode => 2 },
        { name => ':scheme', value => 'http', mode => 2 },
        { name => ':path', value => '/body.txt', mode => 2 },
        { name => ':authority', value => 'localhost', mode => 2 },
        { name => 'accept-encoding', value => $encoding, mode => 2 },
    ]});
    push @requests, [$sid, $encoding];
}
my $frames = $connection->read(all => [
    map { { sid => $_->[0], fin => 1 } } @requests
]);
for my $request (@requests) {
    my ($sid, $encoding) = @$request;
    my ($header) = grep { $_->{type} eq 'HEADERS' && $_->{sid} == $sid } @$frames;
    my $body = join '', map { $_->{data} }
        grep { $_->{type} eq 'DATA' && $_->{sid} == $sid } @$frames;
    is($header->{headers}->{':status'}, 200, "$encoding HTTP/2 status");
    is($header->{headers}->{'content-encoding'}, $encoding,
        "$encoding HTTP/2 encoding");
    my $response = "HTTP/1.1 200 OK\r\nContent-Encoding: $encoding\r\n\r\n$body";
    is(decoded($t, $response, $plain), $plain,
        "$encoding HTTP/2 multiplexed round trip");
}
undef $connection;
$t->stop();
unlike($t->read_file('error.log'), qr/\[(?:alert|emerg)\]|Sanitizer|runtime error:/,
    'no protocol lifecycle errors');
undef $t;
done_testing();
