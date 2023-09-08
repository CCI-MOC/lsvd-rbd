#!/usr/bin/gnuplot -persist

set terminal pdf
set output "iodepth.pdf"

set style line 1 \
    linecolor rgb '#0060ad' \
    linetype 1 linewidth 2 \
    pointtype 7 pointsize 1
set style line 2 \
    linecolor rgb '#dd181f' \
    linetype 1 linewidth 2 \
    pointtype 5 pointsize 1
set datafile separator ","
set autoscale
set xtics format ""
set grid ytics

set ylabel "Throughput (MB/s)

plot 'parse-tmp/iodepth.csv' using 2:xtic(1) title "LSVD" with linespoints ls 1, \
    'parse-tmp/iodepth.csv' using 3 title "RBD" with linespoints ls 2