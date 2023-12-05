
set $dir=/mnt_benchmarks
set $filesize=1g
set $iosize=8k
set $nthreads=1
set $workingset=0
set $directio=1

define file name=largefile1,path=$dir,size=$filesize,prealloc,reuse,paralloc

define process name=rand-write,instances=$nthreads
{
  thread name=rand-thread,memsize=5m,instances=$nthreads
  {
    flowop write name=rand-write1,filename=largefile1,iosize=$iosize,random,workingset=$workingset,directio=$directio
  }
}


echo "Random Write Version 3.0 personality successfully loaded"

run 300