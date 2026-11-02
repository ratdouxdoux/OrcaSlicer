#!/usr/bin/env bash
# Downloads the 3D-printing pages of the Snapmaker wiki into a local text cache
# that knowledge entries are written from. The cache is working material only;
# do not commit it. Usage: fetch_wiki.sh <cache dir>
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cache="${1:?usage: fetch_wiki.sh <cache dir>}"
mkdir -p "$cache/pages"

curl -s -m 120 -X POST https://wiki.snapmaker.com/graphql -H 'Content-Type: application/json' \
  -d '{"query":"{ pages { list(locale:\"en\", limit: 5000) { id path title tags } } }"}' -o "$cache/wiki_index.json"

# Sections for machines Snapmaker Orca supports; laser, CNC, Ray and policy pages are excluded.
perl -MJSON::PP -e '
  local $/; open my $f, "<:raw", $ARGV[0] or die $!;
  my $list = JSON::PP->new->utf8->decode(<$f>)->{data}{pages}{list};
  binmode STDOUT, ":encoding(UTF-8)";
  for my $p (@$list) {
    my $hay = lc join " ", $p->{path}, $p->{title}, @{$p->{tags}};
    next unless $p->{path} =~ m{^(snapmaker_u1|snapmaker_j1|snapmaker_j1_j1s|snapmaker_artisan|Snapmaker_2|general|FAQ|snapmaker_orca|Snapmaker_Orca|full_spectrum_orca|glossary)(/|$)};
    next if $hay =~ m{laser|cnc|rotary|lightburn|fusion|engrav|carv|\bray\b|air.?purifier|bit_assistant|origin_assistant|work_origin|test_|warranty|refund|kickstarter|shipment|backup|/zh/|1064nm|post_process|tool_library};
    (my $title = $p->{title}) =~ s/^\s+|\s+$//g;
    print join("\t", $p->{id}, $p->{path}, $title, join(",", @{$p->{tags}})), "\n";
  }' "$cache/wiki_index.json" > "$cache/selected.tsv"

ok=0; fail=0
while IFS=$'\t' read -r id path title tags; do
  section=$(echo "${path%%/*}" | tr 'A-Z' 'a-z')
  mkdir -p "$cache/pages/$section"
  file="$cache/pages/$section/$id.txt"
  [ -s "$file" ] && { ok=$((ok+1)); continue; }
  url="https://wiki.snapmaker.com/en/$(perl -MURI::Escape -e 'print uri_escape($ARGV[0], "^A-Za-z0-9\-._~/")' "$path")"
  if html=$(curl -sf -m 60 -A "Mozilla/5.0 (Snapmaker Orca knowledge refresh)" "$url") && \
     text=$(printf '%s' "$html" | perl "$here/extract.pl"); then
    { printf 'TITLE: %s\nURL: %s\nTAGS: %s\n\n' "$title" "$url" "$tags"; printf '%s\n' "$text"; } > "$file"
    ok=$((ok+1))
  else
    echo "skipped $url (restricted or empty)"; fail=$((fail+1))
  fi
  sleep 0.3
done < "$cache/selected.tsv"
echo "pages cached: $ok, skipped: $fail"
