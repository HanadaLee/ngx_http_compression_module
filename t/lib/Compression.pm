package Compression;

use strict;
use warnings;

use Exporter 'import';
use Test::More;
use Test::Nginx qw/http http_content/;
use File::Basename qw/dirname/;
use File::Spec;

our @EXPORT = qw/request decoded check_response encodings decodings all_backends
    write_config check_fairness prepare_static check_static/;

sub encodings {
    return split /\s+/, $ENV{TEST_NGINX_COMPRESSION_ENCODINGS}
        // 'zstd br gzip deflate';
}

sub all_backends {
    return scalar(encodings()) == 4;
}

sub decodings {
    return split /\s+/, $ENV{TEST_NGINX_DECOMPRESSION_ENCODINGS}
        // $ENV{TEST_NGINX_COMPRESSION_ENCODINGS} // 'gzip deflate br zstd';
}

sub write_config {
    my ($t, $config) = @_;
    my $encodings = join ' ', encodings();
    $config =~ s/__ENCODINGS__/$encodings/g;
    if (defined $ENV{TEST_NGINX_GZIP} && !$ENV{TEST_NGINX_GZIP}) {
        $config =~ s/^\s*gzip off;\n//mg;
    }
    return $t->write_file_expand('nginx.conf', $config);
}

sub request {
    my ($path, $encoding, $headers, $version) = @_;
    $headers //= '';
    $version //= '1.1';
    my $accept = defined $encoding ? "Accept-Encoding: $encoding\r\n" : '';
    return http("GET $path HTTP/$version\r\nHost: localhost\r\n"
        . "Connection: close\r\n$accept$headers\r\n");
}

sub decoded {
    my ($t, $response, $plain) = @_;
    my ($encoding) = $response =~ /^Content-Encoding:\s*(\S+)\r?$/mi;
    my $data = http_content($response);
    return $data if !defined $encoding;
    $t->write_file('encoded', $data);
    my $dir = $t->testdir();
    my $decoder = File::Spec->rel2abs(dirname(__FILE__) . '/decode.py');
    my $status = system('python3', '-B', $decoder, $encoding,
        "$dir/encoded", "$dir/decoded", length $plain);
    if ($status != 0) {
        fail("$encoding decoder accepts the complete response");
        return '';
    }
    return $t->read_file('decoded');
}

sub check_response {
    my ($t, $response, $encoding, $plain, $name) = @_;
    like($response, qr/^HTTP\/1\.[01] 200 /, "$name status");
    if (defined $encoding) {
        like($response, qr/^Content-Encoding: \Q$encoding\E\r?$/mi,
            "$name encoding");
    } else {
        unlike($response, qr/^Content-Encoding:/mi, "$name identity");
    }
    is(decoded($t, $response, $plain), $plain, "$name round trip");
}

sub check_fairness {
    my ($t, $connection, $plain, $version) = @_;
    my $mode = $version == 2 ? 2 : 4;
    my $scheme = $version == 2 ? 'http' : 'https';
    my $stream = sub {
        my ($path, $encoding) = @_;
        return $connection->new_stream({ headers => [
            { name => ':method', value => 'GET', mode => $mode },
            { name => ':scheme', value => $scheme, mode => $mode },
            { name => ':path', value => $path, mode => $mode },
            { name => ':authority', value => 'localhost', mode => $mode },
            { name => 'accept-encoding', value => $encoding, mode => $mode },
        ]});
    };

    for my $encoding (encodings()) {
        $connection->h2_window(16 * 1024 * 1024) if $version == 2;
        my $large = $stream->('/fair.txt', $encoding);
        my $small = $stream->('/ping', 'identity');
        my $frames = $connection->read(all => [
            { sid => $large, fin => 1 }, { sid => $small, fin => 1 }
        ]);
        my @finished = map { $_->{sid} }
            grep { $_->{flags} & 1 && ($_->{sid} == $large || $_->{sid} == $small) }
            @$frames;
        is($finished[0], $small,
            "$encoding HTTP/$version small response finishes first");
        my ($header) = grep { $_->{type} eq 'HEADERS' && $_->{sid} == $large }
            @$frames;
        is($header->{headers}->{'content-encoding'}, $encoding,
            "$encoding HTTP/$version large encoding");
        my $body = join '', map { $_->{data} }
            grep { $_->{type} eq 'DATA' && $_->{sid} == $large } @$frames;
        is(decoded($t, "HTTP/1.1 200 OK\r\nContent-Encoding: $encoding\r\n\r\n$body",
            $plain), $plain, "$encoding HTTP/$version resumed round trip");

        my $cancel = $stream->('/fair.txt', $encoding);
        $connection->read(all => [{ sid => $cancel, type => 'DATA' }]);
        if ($version == 2) {
            $connection->h2_rst($cancel, 8);
        } else {
            $connection->reset_stream($cancel, 0x10c);
            $connection->stop_sending($cancel, 0x10c);
        }
        my $ping = $stream->('/ping', 'identity');
        $frames = $connection->read(all => [{ sid => $ping, fin => 1 }]);
        my ($reply) = grep { $_->{type} eq 'HEADERS' && $_->{sid} == $ping }
            @$frames;
        is($reply->{headers}->{':status'}, 200,
            "$encoding HTTP/$version survives stream cancellation");
    }
}

sub prepare_static {
    my ($t, $plain) = @_;
    my %suffix = (gzip => 'gz', br => 'br', zstd => 'zst', deflate => 'deflate');
    my %decodings = map { $_ => 1 } decodings();
    mkdir $t->testdir() . '/precompressed';
    my $encoder = File::Spec->rel2abs(dirname(__FILE__) . '/encode.py');
    for my $encoding (grep { $decodings{$_} } encodings()) {
        $t->write_file("precompressed/$encoding.txt", $plain);
        my $path = $t->testdir() . "/precompressed/$encoding.txt";
        is(system('python3', '-B', $encoder, $encoding, $path,
            "$path.$suffix{$encoding}"), 0, "$encoding protocol fixture");
    }
}

sub check_static {
    my ($t, $connection, $plain, $version) = @_;
    my %decodings = map { $_ => 1 } decodings();
    my $mode = $version == 2 ? 2 : 4;
    my $scheme = $version == 2 ? 'http' : 'https';
    $connection->h2_window(1024 * 1024) if $version == 2;
    my @requests;
    for my $encoding (grep { $decodings{$_} } encodings()) {
        for my $accept ($encoding, 'identity') {
            my $sid = $connection->new_stream({ headers => [
                { name => ':method', value => 'GET', mode => $mode },
                { name => ':scheme', value => $scheme, mode => $mode },
                { name => ':path', value => "/precompressed/$encoding.txt", mode => $mode },
                { name => ':authority', value => 'localhost', mode => $mode },
                { name => 'accept-encoding', value => $accept, mode => $mode },
            ]});
            push @requests, [$sid, $encoding, $accept];
        }
    }
    my $frames = $connection->read(all => [
        map { { sid => $_->[0], fin => 1 } } @requests
    ]);
    for my $request (@requests) {
        my ($sid, $encoding, $accept) = @$request;
        my ($header) = grep { $_->{type} eq 'HEADERS' && $_->{sid} == $sid } @$frames;
        my $body = join '', map { $_->{data} }
            grep { $_->{type} eq 'DATA' && $_->{sid} == $sid } @$frames;
        my $name = "$encoding HTTP/$version static $accept";
        is($header->{headers}->{':status'}, 200, "$name status");
        my $expected = $accept eq 'identity' ? undef : $encoding;
        is($header->{headers}->{'content-encoding'}, $expected, "$name encoding");
        is($header->{headers}->{'vary'}, 'Accept-Encoding', "$name Vary");
        my $response = "HTTP/1.1 200 OK\r\n";
        $response .= "Content-Encoding: $encoding\r\n" if defined $expected;
        is(decoded($t, "$response\r\n$body", $plain), $plain, "$name round trip");
    }
}

1;
