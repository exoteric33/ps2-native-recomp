# Prints the value of the first BOOT2 line of a SYSTEM.CNF, read the way scripts/pnach.py reads it: the key in any case,
# spaces or tabs around it and around the "=", an optional byte order mark at the start. The value ends at the first space.
# Line ends (CR LF or CR alone) are turned into LF by the caller: tr '\r' '\n' < SYSTEM.CNF | awk -f boot2.awk
{
  line = $0
  sub(/^(\357\273\277)?[ \t]*/, "", line)
  if (tolower(substr(line, 1, 5)) != "boot2") next
  line = substr(line, 6)
  if (!sub(/^[ \t]*=[ \t]*/, "", line) || line ~ /^[ \t]*$/) next
  sub(/[ \t].*$/, "", line)
  print line
  exit
}
