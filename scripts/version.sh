#!/usr/bin/env bash
# The library version lives in three manifests, one per registry:
#   library.properties (Arduino), library.json (PlatformIO), idf_component.yml (ESP-IDF).
# Keep them in step with this script instead of editing them by hand.
#
#   scripts/version.sh              print the version
#   scripts/version.sh check [TAG]  fail if the manifests disagree, or differ from TAG (a leading "v" is ignored)
#   scripts/version.sh set VERSION  write VERSION to all three manifests
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SEMVER='^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$'

arduino() { sed -n 's/^version=\(.*\)$/\1/p' "$ROOT/library.properties"; }
pio() { sed -n 's/^  "version": "\(.*\)",$/\1/p' "$ROOT/library.json"; }
idf() { sed -n 's/^version: "\(.*\)"$/\1/p' "$ROOT/idf_component.yml"; }

check() {
  local a p i
  a="$(arduino)"; p="$(pio)"; i="$(idf)"
  if [[ -z "$a" || "$a" != "$p" || "$a" != "$i" ]]; then
    echo "version mismatch: library.properties=$a library.json=$p idf_component.yml=$i" >&2
    exit 1
  fi
  if [[ ! "$a" =~ $SEMVER ]]; then
    echo "version $a is not semver (MAJOR.MINOR.PATCH[-PRERELEASE])" >&2
    exit 1
  fi
  if [[ $# -gt 0 && "${1#v}" != "$a" ]]; then
    echo "tag $1 does not match manifest version $a" >&2
    exit 1
  fi
  echo "$a"
}

set_version() {
  local v="$1"
  if [[ ! "$v" =~ $SEMVER ]]; then
    echo "version $v is not semver (MAJOR.MINOR.PATCH[-PRERELEASE])" >&2
    exit 1
  fi
  sed -i "s/^version=.*$/version=$v/" "$ROOT/library.properties"
  sed -i "s/^  \"version\": \".*\",$/  \"version\": \"$v\",/" "$ROOT/library.json"
  sed -i "s/^version: \".*\"$/version: \"$v\"/" "$ROOT/idf_component.yml"
  check
}

case "${1:-}" in
  "") check ;;
  check) shift; check "$@" ;;
  set) [[ $# -eq 2 ]] || { echo "usage: $0 set VERSION" >&2; exit 2; }; set_version "$2" ;;
  *) echo "usage: $0 [check [TAG] | set VERSION]" >&2; exit 2 ;;
esac
