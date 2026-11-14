#!/usr/bin/perl
# Merges <cache dir>/staging/*.json into the shipped knowledge base after validating each entry.
# Usage: perl merge.pl <cache dir> [output file]
# Reports (and drops) invalid entries, and flags entries that share a run of 10 or
# more words with a cached wiki page (<cache dir>/pages), which would indicate copied text.
use strict;
use warnings;
use JSON::PP;
use File::Find;
use FindBin;

my $repo = "$FindBin::Bin/../..";
my ($dir, $output) = @ARGV;
die "usage: merge.pl <cache dir> [output]\n" unless $dir;
$output //= "$repo/resources/ai_knowledge/snapmaker_knowledge.json";
# Setting keys are checked against print_config_def, the definitions AIKnowledgeBase.cpp validates with:
# only PrintConfigDef methods count (not the CLI option definitions), and disabled code is ignored.
my %keys = do {
    open my $f, '<', "$repo/src/libslic3r/PrintConfig.cpp" or die $!;
    local $/; my $source = <$f>;
    $source =~ s{/\*.*?\*/}{}gs;
    $source =~ s{^\s*//.*$}{}gm;
    $source =~ s{^#if 0\b.*?^#endif}{}gms;
    my ($class, %found) = ('');
    for my $line (split /\n/, $source) {
        $class = $1 if $line =~ /^\S.*?\b(\w+)::\w+\s*\(/;
        $found{$1} = 1 if $class eq 'PrintConfigDef' && $line =~ /this->add\("([a-z0-9_]+)"/;
    }
    %found;
};
my %categories = map { $_ => 1 } qw(machine troubleshooting maintenance calibration setting material software safety);
my %machines   = map { $_ => 1 } qw(u1 j1 artisan snapmaker2 any);
my %fields     = map { $_ => 1 } qw(id title category machines tags summary symptoms causes steps settings facts sources);

sub words { my $t = lc shift; $t =~ s/[^a-z0-9]+/ /g; return grep { length } split / /, $t; }

# 10-word shingles of every cached wiki page, plus any web pages saved under <cache dir>/webcache.
my %shingles;
find(sub {
    return unless -f $_ && /\.(txt|html?|md)$/;
    open my $f, '<:encoding(UTF-8)', $_ or return;
    local $/; my $text = <$f>;
    if (/\.html?$/) { $text =~ s{<(script|style)\b.*?</\1>}{ }gsi; $text =~ s{<[^>]+>}{ }gs; }
    my @w = words($text);
    $shingles{join ' ', @w[$_ .. $_ + 9]} = 1 for 0 .. $#w - 9;
}, grep { -d } "$dir/pages", "$dir/webcache");

my (@entries, %seen, @problems, @copied);
my $json = JSON::PP->new->utf8;
for my $file (sort glob("$dir/staging/*.json")) {
    open my $f, '<:raw', $file or die $!;
    local $/; my $raw = <$f>;
    my $data = eval { $json->decode($raw) };
    if (!$data || ref $data ne 'ARRAY') { push @problems, "$file: not a JSON array ($@)"; next; }
    my $count = 0;
    ENTRY: for my $e (@$data) {
        my $id = (ref $e eq 'HASH' && defined $e->{id}) ? $e->{id} : '?';
        my @err;
        if (ref $e ne 'HASH') { push @problems, "$file: non-object entry"; next; }
        for (keys %$e) { push @err, "unknown field $_" unless $fields{$_}; }
        push @err, 'bad id' unless defined $e->{id} && $e->{id} =~ /^[a-z0-9][a-z0-9-]{0,79}$/;
        push @err, 'duplicate id' if defined $e->{id} && $seen{$e->{id}};
        for my $k (qw(title summary)) { push @err, "missing $k" unless defined $e->{$k} && !ref $e->{$k} && length $e->{$k}; }
        push @err, 'title too long'   if defined $e->{title}   && length($e->{title}) > 150;
        push @err, 'summary too long' if defined $e->{summary} && length($e->{summary}) > 1000;
        push @err, 'bad category' unless defined $e->{category} && $categories{$e->{category}};
        my $m = $e->{machines};
        if (ref $m ne 'ARRAY' || !@$m || grep { !defined $_ || !$machines{$_} } @$m) { push @err, 'bad machines'; }
        elsif (@$m > 1 && grep { $_ eq 'any' } @$m) { push @err, 'any mixed with machines'; }
        push @err, 'bad tags' unless ref $e->{tags} eq 'ARRAY' && @{$e->{tags}} && @{$e->{tags}} <= 20
            && !grep { ref $_ || !length $_ || length($_) > 60 } @{$e->{tags}};
        for my $list (qw(symptoms causes steps facts)) {
            next unless exists $e->{$list};
            push @err, "bad $list" unless ref $e->{$list} eq 'ARRAY' && @{$e->{$list}} <= 20
                && !grep { ref $_ || !length $_ || length($_) > 500 } @{$e->{$list}};
        }
        if (exists $e->{settings}) {
            if (ref $e->{settings} ne 'ARRAY' || @{$e->{settings}} > 20) { push @err, 'bad settings'; }
            else {
                for my $s (@{$e->{settings}}) {
                    if (ref $s ne 'HASH' || keys(%$s) != 2 || !defined $s->{key} || !defined $s->{advice}
                        || length($s->{advice}) > 400) { push @err, 'bad setting item'; next; }
                }
                # A reference to a setting this build does not have is dropped, not the whole entry.
                my @known = grep { ref $_ eq 'HASH' && defined $_->{key} && $keys{$_->{key}} } @{$e->{settings}};
                push @problems, "$file: $id: dropped unknown setting " . join(', ', map { $_->{key} // '?' }
                    grep { !(ref $_ eq 'HASH' && defined $_->{key} && $keys{$_->{key}}) } @{$e->{settings}})
                    if @known != @{$e->{settings}} && !@err;
                $e->{settings} = \@known unless @err;
                delete $e->{settings} if !@err && !@known;
            }
        }
        if (ref $e->{sources} ne 'ARRAY' || !@{$e->{sources}} || @{$e->{sources}} > 16) { push @err, 'bad sources'; }
        else {
            for my $s (@{$e->{sources}}) {
                push @err, 'bad source' unless ref $s eq 'HASH' && keys(%$s) == 2 && defined $s->{title} && length $s->{title}
                    && length($s->{title}) <= 200 && defined $s->{url} && $s->{url} =~ m{^https?://} && length($s->{url}) <= 500;
            }
        }
        if (@err) { push @problems, "$file: $id: " . join('; ', @err); next ENTRY; }
        my @text = ($e->{title}, $e->{summary}, map { @{$e->{$_} || []} } qw(symptoms causes steps facts));
        push @text, map { $_->{advice} } @{$e->{settings} || []};
        my @w = words(join ' ', @text);
        for my $i (0 .. $#w - 9) {
            if ($shingles{join ' ', @w[$i .. $i + 9]}) { push @copied, "$id: \"" . join(' ', @w[$i .. $i + 9]) . "\""; last; }
        }
        $seen{$e->{id}} = 1;
        push @entries, $e;
        ++$count;
    }
    printf "%-40s %4d entries\n", ($file =~ m{([^/\\]+)$})[0], $count;
}

my %by_category; $by_category{$_->{category}}++ for @entries;
my %by_machine;  for my $e (@entries) { $by_machine{$_}++ for @{$e->{machines}}; }
print "total ", scalar(@entries), "\n";
print "categories: ", join(', ', map { "$_=$by_category{$_}" } sort keys %by_category), "\n";
print "machines: ",   join(', ', map { "$_=$by_machine{$_}" } sort keys %by_machine), "\n";
print "PROBLEM $_\n" for @problems;
print "COPIED $_\n"  for @copied;

my @order = qw(version description entries id title category machines tags summary symptoms causes steps settings facts sources key advice url);
my %rank; @rank{@order} = 0 .. $#order;
my $out = JSON::PP->new->utf8->pretty->indent_length(1)->sort_by(sub {
    ($rank{$JSON::PP::a} // 99) <=> ($rank{$JSON::PP::b} // 99) or $JSON::PP::a cmp $JSON::PP::b });
my $doc = {
    version     => 1,
    description => 'Curated knowledge for the Snapmaker Orca print assistant. Entries are original summaries of the cited sources '
                 . '(the Snapmaker wiki, public printing references, Snapmaker\'s U1 print quality corpus and its expert '
                 . 'print-quality cases); see each entry\'s sources.',
    entries     => [ sort { $a->{id} cmp $b->{id} } @entries ],
};
open my $o, '>:raw', $output or die $!;
print $o $out->encode($doc);
close $o;
print "wrote $output\n";
