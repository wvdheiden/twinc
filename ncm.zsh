# Source this from ~/.zshrc. Runs ncmac and, on exit, cd's to the directory
# that was active in ncmac when you quit.
ncm() {
  local f
  f=$(mktemp -t ncmac) || return
  command ncmac --lastdir "$f" "$@"
  local d
  d=$(<"$f")
  rm -f "$f"
  [[ -n "$d" && -d "$d" && "$d" != "$PWD" ]] && cd -- "$d"
}
