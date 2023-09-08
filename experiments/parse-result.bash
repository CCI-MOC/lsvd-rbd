#!/bin/bash

[ ! "$2" ] && echo "Usage: parse-result.bash lsvd-output rbd-output" && exit 1
[ ! -f "$1" ] && echo "$1 does not exist" && exit 1
[ ! -f "$2" ] && echo "$2 does not exist" && exit 1

declare -a fio_label=( "randread" "randwrite" "read" "write" )

declare -a iodepth_label=( 1 8 32 64 128 256 )

declare -a fb_label=( "varmail" "fileserver" "oltp" )

plotdir="$(pwd)/gnuplot"
fio_plot="$plotdir/fio.gnu"
iodepth_plot="$plotdir/iodepth.gnu"
fb_plot="$plotdir/filebench.gnu"

tmpdir="$(pwd)/parse-tmp"
fio_file="$tmpdir/fio.csv"
iodepth_file="$tmpdir/iodepth.csv"
fb_file="$tmpdir/filebench.csv"

graph_dir="$(pwd)/graphs"

# fio_parse () {
#     local iops="$(echo "$1" | sed  -e 's/^.*IOPS=\([^,]*\)k.*$/\1/')"
#     local bandwidth="$(echo "$1" | sed -e 's/^.*BW.*(\([^,]*\)MB.*$/\1/')"
# }

rm -rf $tmpdir
mkdir -p $tmpdir

# FIO Varying I/O Types
for i in "${fio_label[@]}"; do
    lsvd="$(grep "^RESULT: Fio (iodepth=256) $i" $1 | sed -e 's/^.*BW.*(\([^,]*\)MB.*/\1/')"
    rbd="$(grep "^RESULT: Fio (iodepth=256) $i" $2 | sed -e 's/^.*BW.*(\([^,]*\)MB.*/\1/')"
    echo "$i, $lsvd, $rbd" >> $fio_file
done

# FIO random write varying io_depths
for i in "${iodepth_label[@]}"; do
    lsvd="$(grep "^RESULT: Fio (iodepth=$i) randwrite" $1 | sed -e 's/^.*BW.*(\([^,]*\)MB.*/\1/')"
    rbd="$(grep "^RESULT: Fio (iodepth=$i) randwrite" $2 | sed -e 's/^.*BW.*(\([^,]*\)MB.*/\1/')"
    echo "$i, $lsvd, $rbd" >> $iodepth_file
done

# Filebench
for i in "${fb_label[@]}"; do
    lsvd="$(grep "^RESULT.*$i" $1 | sed -e 's/^.*wr \([^,]*\)mb.*$/\1/')"
    rbd="$(grep "^RESULT.*$i" $2 | sed -e 's/^.*wr \([^,]*\)mb.*$/\1/')"
    echo "$i, $lsvd, $rbd" >> $fb_file
done

gnuplot $fio_plot
gnuplot $iodepth_plot
gnuplot $fb_plot

mkdir -p $graph_dir
mv *.pdf $graph_dir