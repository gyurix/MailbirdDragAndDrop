#!/usr/bin/env bash
set -euo pipefail

usage() {
  printf 'Usage: %s {apply|verify|restore} /path/to/Mail.dll\n' "$0" >&2
  exit 2
}

[[ $# -eq 2 ]] || usage
mode=$1
assembly=$2
[[ -f "$assembly" ]] || { printf 'Assembly not found: %s\n' "$assembly" >&2; exit 1; }

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source="$root/tools/MailbirdSocketPatch.cs"
cecil=''
for candidate in \
  /usr/lib/mono/gac/Mono.Cecil/*/Mono.Cecil.dll \
  /usr/lib/mono/4.5/Mono.Cecil.dll \
  /usr/lib/mono/4.0/Mono.Cecil.dll; do
  if [[ -f "$candidate" ]]; then cecil=$candidate; break; fi
done
[[ -n "$cecil" ]] || { printf 'Mono.Cecil not found; install libmono-cecil-cil.\n' >&2; exit 1; }
command -v mcs >/dev/null || { printf 'mcs not found; install mono-devel.\n' >&2; exit 1; }
command -v mono >/dev/null || { printf 'mono not found; install mono-runtime.\n' >&2; exit 1; }

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
mcs -nologo -warn:4 -r:"$cecil" "$source" -out:"$tmpdir/MailbirdSocketPatch.exe"

case "$mode" in
  apply)
    mono "$tmpdir/MailbirdSocketPatch.exe" apply "$assembly"
    ;;
  verify)
    mono "$tmpdir/MailbirdSocketPatch.exe" verify "$assembly"
    ;;
  restore)
    mono "$tmpdir/MailbirdSocketPatch.exe" restore "$assembly"
    ;;
  *) usage ;;
esac
