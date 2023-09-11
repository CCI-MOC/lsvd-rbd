#!/usr/bin/gnuplot -persist

set terminal pdf
set output "filebench.pdf"

set style line 2 lc rgb 'black' lt 1 lw 1
set style data histogram
set style histogram cluster gap 1
set style fill pattern border -1
set datafile separator ","
set autoscale
set boxwidth 0.9
set xtics format ""
set grid ytics

set ylabel "Throughput (MB/s)

plot 'parse-tmp/filebench.csv' using 2:xtic(1) title "LSVD" ls 2, \
    'parse-tmp/filebench.csv' using 3 title "RBD" ls 2