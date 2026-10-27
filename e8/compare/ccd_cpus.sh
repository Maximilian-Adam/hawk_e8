#!/bin/sh
# The pinned CPU lists of the two bare-metal run sets
# (docs/measurement_runbook.md), from the L3 topology in sysfs: one CPU per core
# (the core's lowest-numbered online thread), grouped by L3 domain (CCD);
# the V-cache CCD is the one with the larger L3.
#
#   ccd_cpus.sh headline    the V-cache CCD's cores, e.g. 0,1,2,3,4,5,6,7
#   ccd_cpus.sh cross-ccd   the V-cache CCD's cores, then the other CCD's
#   ccd_cpus.sh show        every L3 domain: id, size, cores
#
# Exit status 1 (with a message) unless there are exactly two L3 domains
# of different sizes with the same number of cores (as on the 9950X3D);
# WSL shows a single L3 domain. CPU_SYSFS overrides
# /sys/devices/system/cpu (for testing).

root=${CPU_SYSFS:-/sys/devices/system/cpu}
what=${1:-show}
case "$what" in
headline|cross-ccd|show) ;;
*) echo "usage: ccd_cpus.sh headline|cross-ccd|show" >&2; exit 2 ;;
esac

# expand LIST: "0-3,8" -> one number per line.
expand() {
	echo "$1" | tr ',' '\n' | while read -r r; do
		case "$r" in
		*-*) seq "${r%-*}" "${r#*-}" ;;
		'') ;;
		*) echo "$r" ;;
		esac
	done
}

if [ ! -r "$root/online" ]; then
	echo "ccd_cpus.sh: $root/online not readable" >&2
	exit 1
fi
# "l3_id l3_size_kib cpu" for the first online thread of every core.
domains=$(for c in $(expand "$(cat "$root/online")"); do
	d=$root/cpu$c
	if [ ! -r "$d/cache/index3/id" ] || [ ! -r "$d/cache/index3/size" ]
	then
		echo "ccd_cpus.sh: no L3 information for cpu$c" >&2
		exit 1
	fi
	first=$(expand "$(cat "$d/topology/thread_siblings_list")" \
		| sort -n | head -n 1)
	[ "$first" = "$c" ] || continue
	size=$(cat "$d/cache/index3/size")
	case "$size" in
	*K) size=${size%K} ;;
	*M) size=$(( ${size%M} * 1024 )) ;;
	esac
	echo "$(cat "$d/cache/index3/id") $size $c"
done) || exit 1

printf '%s\n' "$domains" | awk -v what="$what" '
	{
		if (!($1 in size)) { ids[++nd] = $1; size[$1] = $2 }
		cpus[$1] = cpus[$1] (cpus[$1] == "" ? "" : ",") $3
		ncore[$1]++
	}
	END {
		if (what == "show") {
			for (i = 1; i <= nd; i++)
				printf "L3 %s: %d KiB, %d cores: %s\n", ids[i],
					size[ids[i]], ncore[ids[i]], cpus[ids[i]]
		}
		if (nd != 2 || size[ids[1]] == size[ids[2]] \
			|| ncore[ids[1]] != ncore[ids[2]]) {
			printf "ccd_cpus.sh: expected two L3 domains (CCDs) of" \
				" different sizes with equal core counts, found" \
				" %d:", nd > "/dev/stderr"
			for (i = 1; i <= nd; i++)
				printf " %s (%d KiB, %d cores)", ids[i],
					size[ids[i]], ncore[ids[i]] > "/dev/stderr"
			printf "\n" > "/dev/stderr"
			exit 1
		}
		v = size[ids[1]] > size[ids[2]] ? ids[1] : ids[2]
		o = v == ids[1] ? ids[2] : ids[1]
		if (what == "show")
			printf "V-cache CCD: L3 %s\nheadline: %s\ncross-ccd: %s,%s\n",
				v, cpus[v], cpus[v], cpus[o]
		else if (what == "headline")
			print cpus[v]
		else
			print cpus[v] "," cpus[o]
	}'
