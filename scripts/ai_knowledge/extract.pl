#!/usr/bin/perl
# Extracts the rendered Wiki.js page body (<template slot="contents">) as plain text
# with light structure: headings, list items, table cells and image alt text.
use strict;
use warnings;
use HTML::Entities qw(decode_entities);

local $/;
my $html = <STDIN>;
binmode STDOUT, ':encoding(UTF-8)';
utf8::decode($html);
my ($body) = $html =~ m{<template slot="contents">(.*?)</template>}s;
exit 1 unless defined $body;

$body =~ s{<(script|style)\b.*?</\1>}{}gsi;
$body =~ s{<img\b[^>]*\balt="([^"]+)"[^>]*>}{ [image: $1] }gsi;
$body =~ s{<h([1-6])\b[^>]*>}{"\n\n" . ('#' x $1) . ' '}gsie;
$body =~ s{</h[1-6]>}{\n}gsi;
$body =~ s{<li\b[^>]*>}{\n- }gsi;
$body =~ s{<(?:td|th)\b[^>]*>}{ | }gsi;
$body =~ s{</tr>}{\n}gsi;
$body =~ s{<br\s*/?>}{\n}gsi;
$body =~ s{</(?:p|div|ul|ol|table|blockquote|pre)>}{\n}gsi;
$body =~ s{<[^>]+>}{}gs;
$body = decode_entities($body);
$body =~ s{\x{b6}}{}g;            # Wiki.js heading anchors
$body =~ s{[ \t\x{a0}]+}{ }g;
$body =~ s{ *\n *}{\n}g;
$body =~ s{\n{3,}}{\n\n}g;
$body =~ s{^\s+|\s+$}{}g;
print $body, "\n";
