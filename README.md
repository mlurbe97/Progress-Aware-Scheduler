# Progress-Aware-Scheduler

My own interpretation of the Progress Aware Scheduler porposed by Josué Feliu.

## Implementation author

* Manel Lurbe Sempere (malursem@inf.upv.es)

## Original documentation from the authors

- [Addressing Fairness in SMT Multicores with a Progress-Aware Scheduler.](https://ieeexplore.ieee.org/document/7161508)

## Compile and run

- Tested on IBM POWER 8 with Ubuntu 18.04.

- The code is ready to run SPEC CPU Benchmarks, install them and change the path to the benchmarks in the scheduler code before compile.

- Use [libperf](doc/lib) library for C to compile the scheduler [PAS.c](src/PAS.c).

- To run the scheduler just launch this script as super user -> [launch_pas](doc/launch_scripts/launch_pas).