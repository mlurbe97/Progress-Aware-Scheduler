#!/bin/bash

## Progress-Aware-Scheduler implementation.
## Year: 2021
## Author: Manel Lurbe Sempere <malursem@gap.upv.es>.

for cpu in $(seq 0 79);
do
	{ sudo cpufreq-set -g userspace -c $cpu; }
done;

{ sudo cpupower frequency-set -f 3690000; }

echo "CPU Ready..."