set term pdf size 6in,4in
set out "QEMU-Store-instrumentation-overhead.pdf"
set sty data hist
set xtic rotate by -40
set ylabel 'Overhead(percentage over native)'
set grid ytics lw 2
set tics nomirror
set xtics scale 0
set k left
#set yrange [0:0.7]
plot "QEMU-Store-instrumentation-overhead.dat" using 2:xtic(1) ti col fill pattern 0 black#, for [i=3:8] '' us i ti col fill pattern i-2 black

