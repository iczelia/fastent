# Regenerates doc/bench.png from doc/bench.tsv.
#   gnuplot doc/bench.gnuplot
#   gnuplot -e "data='doc/bench.tsv'; out='doc/bench.png'" doc/bench.gnuplot
# Overridable: host='...', fontspec='Misc Fixed', the plotted level.

if (!exists("data"))     data     = 'doc/bench.tsv'
if (!exists("out"))      out      = 'doc/bench.png'
if (!exists("fontspec")) fontspec = 'medium'
if (!exists("host"))     host     = 'Ryzen 9 5950X, 16C/32T, dual-channel DDR4'
modes  = "byte bit byte-fold"
levels = "plain ee"
mlabel(m) = (m eq "byte-fold") ? "-f" : (m eq "bit") ? "-b" : (m eq "byte") ? "" : m
llabel(l) = (l eq "plain") ? "plain/-e" : l
psym(l)   = (l eq "plain") ? 7 : (l eq "ee") ? 9 : 13

ds = system(sprintf("awk '$3==\"plain\"&&($2==\"byte\"||$2==\"bit\"||$2==\"byte-fold\")&&$4~/^[0-9]+$/{print $1}' %s | sort -u | tr '\\n' ' '", data))
nds  = words(ds)
size = system(sprintf("awk '/size\\/run/{print $3}' %s", data))

medline(m, l) = sprintf("< awk -v M=%s -v L=%s '$2==M&&$3==L&&$4~/^[0-9]+$/{n[$4]++;A[$4,n[$4]]=$9} END{c=\"1 2 4 8 16 32\";split(c,J,\" \");for(i=1;i<=6;i++){j=J[i];g=n[j];for(p=1;p<=g;p++)t[p]=A[j,p];for(a=2;a<=g;a++){x=t[a];b=a-1;while(b>=1&&t[b]>x){t[b+1]=t[b];b--}t[b+1]=x}if(g)print j,(g%%2)?t[(g+1)/2]:(t[g/2]+t[g/2+1])/2}}' %s", m, l, data)
dsline(m, l, d) = sprintf("< awk -v M=%s -v L=%s -v D=%s '$1==D&&$2==M&&$3==L&&$4~/^[0-9]+$/{print $4,$9}' %s | sort -n", m, l, d, data)
ebase(m) = real(system(sprintf("awk -v M=%s '$2==M&&$3==\"plain\"&&$4==\"ent\"{v[++n]=$9} END{for(a=2;a<=n;a++){x=v[a];b=a-1;while(b>=1&&v[b]>x){v[b+1]=v[b];b--}v[b+1]=x}print n?((n%%2)?v[(n+1)/2]:(v[n/2]+v[n/2+1])/2):0}' %s", m, data)))

cbold(m) = (m eq "byte") ? "#1f77b4" : (m eq "bit") ? "#d62728" : (m eq "byte-fold") ? "#2ca02c" : "#ff7f0e"
cthin(m) = (m eq "byte") ? "#aec7e8" : (m eq "bit") ? "#ff9896" : (m eq "byte-fold") ? "#98df8a" : "#ffbb78"

eval sprintf("set terminal png %s size 700,510 noenhanced", fontspec)
set output out

set title sprintf("fastent on %s\n%d datasets x %s MiB; color=mode symbol=level; bold=median thin=per-dataset dashed=ent(1)", host, nds, size)
set xlabel "-j worker threads (log2)" offset 0,0.5
set ylabel "Throughput, MiB/s (log10)"
set logscale x 2
set logscale y 10
set xtics (1, 2, 4, 8, 16, 32)
set ytics (50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000)
set yrange [48:22000]
set grid xtics ytics lt 0 lc rgb "#c0c0c0"
set xrange [0.82:39]
set bmargin 10
set key at screen 0.5, 0.125 center horizontal maxcols 4 box spacing 1.1 width 1 height 1
set label 1 "plain modes equivalent to ent(1), -ee and FIPS are (slower) supersets of ent" \
  at screen 0.5, 0.025 center

eb_byte = ebase("byte")
eb_bit  = ebase("bit")
eb_fold = ebase("byte-fold")
set arrow from 1,eb_byte to 32,eb_byte nohead dt 2 lw 1 lc rgb cbold("byte")
set arrow from 1,eb_bit  to 32,eb_bit  nohead dt 2 lw 1 lc rgb cbold("bit")
set arrow from 1,eb_fold to 32,eb_fold nohead dt 2 lw 1 lc rgb cbold("byte-fold")

plot \
  for [m in modes] for [l in levels] for [d in ds] dsline(m, l, d) \
       using 1:2 with lines lw 1 lc rgb cthin(m) notitle, \
  for [d in ds] dsline("fips", "-", d) using 1:2 with lines \
       lw 1 lc rgb cthin("fips") notitle, \
  for [m in modes] for [l in levels] medline(m, l) using 1:2 \
       with linespoints lw 2 pt psym(l) ps 1.1 lc rgb cbold(m) \
       title (mlabel(m) eq "" ? llabel(l) : sprintf("%s %s", mlabel(m), llabel(l))), \
  medline("fips", "-") using 1:2 with linespoints lw 2 pt 13 ps 1.1 \
       lc rgb cbold("fips") title "fips"
