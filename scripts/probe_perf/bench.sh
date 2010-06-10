# Measure probe performance.  Currently measures: 
# static user uprobes, static user kprobes, dynamic user uprobes.

# example use:
# ./bench.sh -stapdir /foo/stap/install/ -gccdir /foo/gcc-4.4.3-10/install/

function stap_test() {

# Compile bench
$STAP/bin/dtrace -G -s bench_.d
$STAP/bin/dtrace --types -h -s bench_.d
if [ "$3"x = "semx" ] ; then
   IMPLICIT_ENABLED="-DSTAP_SDT_IMPLICIT_ENABLED"
else
   IMPLICIT_ENABLED=""
fi
$GCC/bin/gcc -D$1 -DLOOP=10 bench_.o bench.c -o bench-$2$3.x -I. -I$STAP/include -g $IMPLICIT_ENABLED
if [ $? -ne 0 ]; then echo "error compiling bench-$2$3"; return; fi
./bench-$2$3.x > /dev/null

# Compile stapbenchmod
$STAP/bin/stap -DSTP_NO_OVERLOAD=1 -g -p4 -m stapbench_$2$3 bench.stp ./bench-$2$3.x $1 >/dev/null
if [ $? -ne 0 ]; then echo "error compiling stapbench_$2$3"; return; fi

# Parse /usr/bin/time, bench.x, bench.stp output to get statistics
(
taskset 1 /usr/bin/time ./bench-$2$3.x 2>&1 >/dev/null # mute stdout, get times from stderr
$STAP/bin/staprun stapbench_$2$3.ko -c "taskset 1 /usr/bin/time ./bench-$2$3.x" 2>&1
) | awk --non-decimal-data '
function seconds(s) {
    if (index(s,":"))
	m=substr(s,0,index(s,":"))*60
    else m=0
    return m + substr(s,index(s,":")+1) 
}

# probe count and average probe setup cycles from bench.stp
/@count/ {
  n += 1
  count += (substr($2,8));
  avg += (substr($6,6))
}

# elapsed time from /usr/bin/time
/elapsed/ {
  elapsed=(seconds($3))
  if (nostapet == 0) {
    nostapet=elapsed
    print "without stap elapsed time is " elapsed
  }
  else
    print "with stap elapsed time is " elapsed
}

# average probe cycles from bench.x
/_cycles/ {
  cycles_n += 1
  cycles += $2
}

END {
  print "count of probe hits is " count
  if (cycles && n) {
    printf "average cycles/probe is %d\n", (cycles / cycles_n)
    printf "average setup cycles/probe is %d\n", (avg / n)
  }
  if (count)
    printf "seconds/probe (%s/%s) is %.9f\n",
      elapsed-nostapet, count, (elapsed-nostapet)/count
}'

}

# Main

while test ! -z "$1" ; do
    if [ "$1" = "-gccdir" ] ; then GCC=$2 ; shift
    elif [ "$1" = "-stapdir" ] ; then STAP=$2 ; shift
    elif [ "$1" = "-k" ] ; then KEEP=1 ;
    elif [ "$1" = "-h" -o "$1" = "-help" -o "$1" = "?" ] ; then
        echo 'Usage $0 [-k] [-stapdir /stap/top/dir] [-gccdir /gcc/top/dir] [-help]'
        exit
    else echo Unrecognized arg "$1" 
        exit
    fi
   shift
done

if [ ! -z "$GCC" ] ; then
 if [ ! -x "$GCC/bin/gcc" ] ; then
    echo $GCC/bin/gcc does not exist
    exit
 fi
else
 GCC=/usr/
 echo Using /usr/bin/gcc
fi

if [ ! -z "$STAP" ] ; then
 if [ ! -x "$STAP/bin/stap" ] ; then
    echo $STAP/bin/stap does not exist
    exit
 fi
else
 STAP=/usr/
 echo Using /usr/bin/stap
fi

echo -e "\n##### NO SDT #####\n"
stap_test NO_STAP_SDT nosdt

echo -e "\n##### KPROBE SEM #####\n"
stap_test EXPERIMENTAL_KPROBE_SDT kprobe sem

echo -e "\n##### KPROBE NO SEM #####\n"
stap_test EXPERIMENTAL_KPROBE_SDT kprobe

echo -e "\n##### UPROBE SEM #####\n"
stap_test UPROBE_SDT uprobe sem

echo -e "\n##### UPROBE NO SEM #####\n"
stap_test UPROBE_SDT uprobe

echo -e "\n##### UPROBE V2 SEM #####\n"
stap_test STAP_SDT_V2 uprobe2 sem

echo -e "\n##### UPROBE V2 NO SEM #####\n"
stap_test STAP_SDT_V2 uprobe2

