# Shell integration for twinc (bash and zsh). Source it from ~/.bashrc or ~/.zshrc:
#   source /path/to/share/twinc/ncm.sh
# Then `ncm` runs twinc and, when you quit, cd's to the directory that was
# active in twinc.
ncm() {
  local f d
  f=$(mktemp "${TMPDIR:-/tmp}/twinc.XXXXXX") || return
  command twinc --lastdir "$f" "$@"
  d=$(cat -- "$f" 2>/dev/null)
  rm -f -- "$f"
  if [ -n "$d" ] && [ -d "$d" ] && [ "$d" != "$PWD" ]; then cd -- "$d" || return; fi
}
