# Source this from ~/.zshrc. Runs twinc and, on exit, cd's to the directory
# that was active in twinc when you quit.
ncm() {
  local f
  f=$(mktemp -t twinc) || return
  command twinc --lastdir "$f" "$@"
  local d
  d=$(<"$f")
  rm -f "$f"
  [[ -n "$d" && -d "$d" && "$d" != "$PWD" ]] && cd -- "$d"
}
