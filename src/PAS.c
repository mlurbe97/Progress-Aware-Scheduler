/*
	Progress-Aware-Scheduler implementation
	Author: Manel Lurbe Sempere
	Year: 2021

*/

/*************************************************************
 **                  Includes                               **
 *************************************************************/

#include <sys/types.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <err.h>
#include <sys/poll.h>
#include <sched.h>
#include <time.h>

#include "perf_util.h"


/*************************************************************
 **                   Defines                               **
 *************************************************************/

#define N_MAX 10// Number of processor cores without counting SMT logical cores.
#define PTRACE_DSCR 44

/*************************************************************
 **                   Structs                               **
 *************************************************************/

typedef struct {
	char *events;//String of events to be measured on core counters.
	int delay;//Quantum duration (ms).
	int pinned;//CPU Pinned.
	int group;
	char *output_directory;
} options_t;

typedef struct {//Defines the variables of an application or process. Represents a core of the processor.
	pid_t pid;//Process id.
	int benchmark;//Application to run.
	int finished;//Have you completed the target instructions?
	int status;//Application or core status.
	int core;// Core where it runs.
	uint64_t counters [45];//Counters for fds.
	perf_event_desc_t *fds;
	int num_fds;//Number of counters
	cpu_set_t mask;
	char *core_out;//Output directory for core info.
	/*
		To save the information of each application in each quantum.
	*/
	uint64_t total_instructions;//Total instructions executed by the application.
	uint64_t total_cycles;//Total cycles executed by the application.
	uint64_t *my_Counters;//Counter array where the different counters are stored in each quantum.
	uint64_t a_instructions;//Instructions executed in the last quantum.
	uint64_t a_cycles;//Cycles executed in the last quantum.
	int actual_DSCR;//Current dscr configuration.
	float actual_IPC;//IPC of the measurement interval.
	float predicted_IPC;//IPC predicted by the predictor.
	float actual_BW;
	float sum_BW;

	//For the previous intervals we keep the CPI reached.
	float last_IPC_1;
	float last_IPC_2;
	float last_IPC_3;

	//For the previous intervals we save the consumed BW
	float last_BW_1;
	float last_BW_2;
	float last_BW_3;

	// Is relaunched
	int relauched;
} node;

/*************************************************************
 **                   Variables Globales                    **
 *************************************************************/

node queue [N_MAX];//Queue where all running applications are stored. They are the cores of the processor.
static options_t options;

int end_experiment = 0;
int N;//Number of running applications
int workload;//Workload number

float sum_BW;

int planificar = 0;//Indicates whether to plan when it is 1, when all quantums are executed virtual_Quantums.
int time_to_predict = 0;
const int virtual_Quantums = 30;//Number of quantums that make up a virtual quantum. 200 ms x 30 quanta = 6s
const int num_Counters = 2;//Indicates the number of counters that are programmed, 4 per quantum. Instructions and cycles go apart.
const float CPU_FREQ = 3690;//Processor frequency at which we launch the experiments.
const int LLC_LOAD_MISSES = 0;//Counter position in the array of counters.
const int PM_MEM_PREF = 1;//Counter position in the array of counters.

//Reading and writing files.
FILE* core_Info;

// Overhead time
//clock_t overhead;

/*************************************************************
 **                   Benchmarks Spec2006-2017              **
 *************************************************************/

char *benchmarks[][200] = {
    // 0 -> perlbench
    {NULL, NULL, NULL},
    // 1 -> bzip2
    {"/home/malursem/working_dir/spec_bin/bzip2.ppc64", "/home/malursem/working_dir/CPU2006/401.bzip2/data/all/input/input.combined", "200", NULL},
    // 2 -> gcc
    {"/home/malursem/working_dir/spec_bin/gcc.ppc64", "/home/malursem/working_dir/CPU2006/403.gcc/data/ref/input/scilab.i", "-o", "scilab.s", NULL},
    // 3 -> mcf
    {"/home/malursem/working_dir/spec_bin/mcf.ppc64", "/home/malursem/working_dir/CPU2006/429.mcf/data/ref/input/inp.in", NULL},
    // 4 -> gobmk [Doesn't work]
    {"/home/malursem/working_dir/spec_bin/gobmk.ppc64", "--quiet", "--mode", "gtp", NULL},
    // 5 -> hmmer
    {"/home/malursem/working_dir/spec_bin/hmmer.ppc64", "--fixed", "0", "--mean", "500", "--num", "500000", "--sd", "350", "--seed", "0", "/home/malursem/working_dir/CPU2006/456.hmmer/data/ref/input/retro.hmm", NULL},
    // 6 -> sjeng
    {"/home/malursem/working_dir/spec_bin/sjeng.ppc64", "/home/malursem/working_dir/CPU2006/458.sjeng/data/ref/input/ref.txt", NULL},
    // 7 -> libquantum
    {"/home/malursem/working_dir/spec_bin/libquantum.ppc64", "1397", "8", NULL},
    // 8 -> h264ref
    {"/home/malursem/working_dir/spec_bin/h264ref.ppc64", "-d", "/home/malursem/working_dir/CPU2006/464.h264ref/data/ref/input/foreman_ref_encoder_baseline.cfg", NULL},
    // 9 -> omnetpp
    {"/home/malursem/working_dir/spec_bin/omnetpp.ppc64", "/home/malursem/working_dir/CPU2006/471.omnetpp/data/ref/input/omnetpp.ini", NULL},
    // 10 -> astar
    {"/home/malursem/working_dir/spec_bin/astar.ppc64", "/home/malursem/working_dir/CPU2006/473.astar/data/ref/input/BigLakes2048.cfg", NULL},
    // 11 -> xalancbmk
    {"/home/malursem/working_dir/spec_bin/Xalan.ppc64", "-v", "/home/malursem/working_dir/CPU2006/483.xalancbmk/data/ref/input/t5.xml", "/home/malursem/working_dir/CPU2006/483.xalancbmk/data/ref/input/xalanc.xsl", NULL},
    // 12 -> bwaves
    {"/home/malursem/working_dir/spec_bin/bwaves.ppc64", NULL},
    // 13 -> gamess
    {"/home/malursem/working_dir/spec_bin/gamess.ppc64", NULL},
    // 14 -> milc
    {"/home/malursem/working_dir/spec_bin/milc.ppc64", NULL},
    // 15 -> zeusmp
    {"/home/malursem/working_dir/spec_bin/zeusmp.ppc64", NULL},
    // 16 -> gromacs
    {"/home/malursem/working_dir/spec_bin/gromacs.ppc64", "-silent", "-deffnm", "/home/malursem/working_dir/CPU2006/435.gromacs/data/ref/input/gromacs", "-nice", "0", NULL},
    // 17 -> cactusADM
    {"/home/malursem/working_dir/spec_bin/cactusADM.ppc64", "/home/malursem/working_dir/CPU2006/436.cactusADM/data/ref/input/benchADM.par", NULL},
    // 18 -> leslie3d
    {"/home/malursem/working_dir/spec_bin/leslie3d.ppc64", NULL},
    // 19 -> namd
    {"/home/malursem/working_dir/spec_bin/namd.ppc64", "--input", "/home/malursem/working_dir/CPU2006/444.namd/data/all/input/namd.input", "--iterations", "38", "--output", "namd.out", NULL},
    // 20 -> microbench
    {"/home/malursem/working_dir/microbench", "100", "0", "1024", "0"},
    // 21 -> soplex
    {"/home/malursem/working_dir/spec_bin/soplex.ppc64", "-s1", "-e","-m45000", "/home/malursem/working_dir/CPU2006/450.soplex/data/ref/input/pds-50.mps", NULL},
    //22 -> povray
    {"/home/malursem/working_dir/spec_bin/povray.ppc64", "/home/malursem/working_dir/CPU2006/453.povray/data/ref/input/SPEC-benchmark-ref.ini", NULL},
    // 23 -> GemsFDTD
    {"/home/malursem/working_dir/spec_bin/GemsFDTD.ppc64", NULL},
    // 24 -> lbm
    {"/home/malursem/working_dir/spec_bin/lbm.ppc64", "300", "reference.dat", "0", "1", "/home/malursem/working_dir/CPU2006/470.lbm/data/ref/input/100_100_130_ldc.of", NULL},
    // 25 -> tonto
    {"/home/malursem/working_dir/spec_bin/tonto.ppc64", NULL},
    // 26 -> calculix
    {"/home/malursem/working_dir/spec_bin/calculix.ppc64", "-i", "/home/malursem/working_dir/CPU2006/454.calculix/data/ref/input/hyperviscoplastic", NULL},
    // 27
    {NULL, NULL, NULL},
    // 28
    {NULL, NULL, NULL},
    // 29
    {NULL, NULL, NULL},
    //* SPEC CPU 2017 *//
    // 30 -> perlbench_r checkspam
    {"/home/malursem/working_dir_test/spec_bin2017/perlbench_r.ppc64", "-I/home/malursem/working_dir_test/CPU2017/500.perlbench_r/lib", "/home/malursem/working_dir_test/CPU2017/500.perlbench_r/checkspam.pl", "2500", "5", "25", "11", "150", "1", "1", "1", "1", NULL},
    // 31 -> gcc_r
    {"/home/malursem/working_dir/CPU2017_bin/cpugcc_r.ppc64", "/home/malursem/working_dir/CPU2017/502.gcc_r/gcc-smaller.c", "-O3", "-fipa-pta", "-o", "gcc-smaller.opts-O3_-fipa-pta.s", NULL},
    // 32 -> mcf_s
    {"/home/malursem/working_dir/CPU2017_bin/mcf_s.ppc64", "/home/malursem/working_dir/CPU2017/605.mcf_s/inp.in", NULL},
    // 33 -> omnetpp_s
    {"/home/malursem/working_dir/CPU2017_bin/omnetpp_s.ppc64", "-c", "General", "-r", "0", NULL},
    // 34 -> xalancbmk_s
    {"/home/malursem/working_dir/CPU2017_bin/xalancbmk_s.ppc64", "-v", "/home/malursem/working_dir/CPU2017/623.xalancbmk_s/t5.xml", "/home/malursem/working_dir/CPU2017/623.xalancbmk_s/xalanc.xsl", NULL},
    // 35 -> x264_s
    {"/home/malursem/working_dir/CPU2017_bin/x264_s.ppc64", "--pass", "1", "--stats", "x264_stats.log", "--bitrate", "1000", "--frames", "1000", "-o", "/home/malursem/working_dir/CPU2017/625.x264_s/BuckBunny_New.264", "/home/malursem/working_dir/CPU2017/625.x264_s/BuckBunny.yuv", "1280x720", NULL},
    // 36 -> deepsjeng_r
    {"/home/malursem/working_dir/CPU2017_bin/deepsjeng_r.ppc64", "/home/malursem/working_dir/CPU2017/531.deepsjeng_r/ref.txt", NULL},
    // 37 -> leela_s
    {"/home/malursem/working_dir/CPU2017_bin/leela_s.ppc64", "/home/malursem/working_dir/CPU2017/641.leela_s/ref.sgf", NULL},
    // 38 -> exchange2_s
    {"/home/malursem/working_dir/CPU2017_bin/exchange2_s.ppc64", "6", NULL},
    // 39 -> xz_r 1
    {"/home/malursem/working_dir/CPU2017_bin/xz_r.ppc64", "/home/malursem/working_dir/CPU2017/557.xz_r/cld.tar.xz", "160", "19cf30ae51eddcbefda78dd06014b4b96281456e078ca7c13e1c0c9e6aaea8dff3efb4ad6b0456697718cede6bd5454852652806a657bb56e07d61128434b474", "59796407", "61004416", "6", NULL},
    // 40 -> bwaves_r
    {"/home/malursem/working_dir/CPU2017_bin/bwaves_r.ppc64", NULL},
    // 41 -> cactuBSSN_r
    {"/home/malursem/working_dir/CPU2017_bin/cactuBSSN_r.ppc64", "/home/malursem/working_dir/CPU2017/507.cactuBSSN_r/spec_ref.par", NULL},
    // 42 -> lbm_r
    {"/home/malursem/working_dir/CPU2017_bin/lbm_r.ppc64", "3000", "reference.dat", "0", "0", "/home/malursem/working_dir/CPU2017/519.lbm_r/100_100_130_ldc.of", NULL},
    // 43 -> wrf_s
    {"/home/malursem/working_dir/CPU2017_bin/wrf_s.ppc64", NULL},
    // 44 -> pop2_s
    {"/home/malursem/working_dir/CPU2017_bin/speed_pop2.ppc64", NULL},
    // 45 -> imagick_r
    {"/home/malursem/working_dir/CPU2017_bin/imagick_r.ppc64", "-limit", "disk", "0", "/home/malursem/working_dir/CPU2017/538.imagick_r/refrate_input.tga", "-edge", "41", "-resample", "181%", "-emboss", "31", "-colorspace", "YUV", "-mean-shift", "19x19+15%", "-resize", "30%", "refrate_output.tga", NULL},
    // 46 -> nab_s
    {"/home/malursem/working_dir/CPU2017_bin/nab_s.ppc64", "3j1n", "20140317", "220", NULL},
    // 47 -> fotonik3d_r
    {"/home/malursem/working_dir/CPU2017_bin/fotonik3d_r.ppc64", NULL},
    // 48 -> roms_r
    {"/home/malursem/working_dir/CPU2017_bin/roms_r.ppc64", NULL},
    // 49 -> namd_r
    {"/home/malursem/working_dir/CPU2017_bin/namd_r.ppc64", "--input", "/home/malursem/working_dir/CPU2017/508.namd_r/apoa1.input", "--output", "apoa1.ref.output", "--iterations", "65", NULL},
    // 50 -> parest_r
    {"/home/malursem/working_dir/CPU2017_bin/parest_r.ppc64", "/home/malursem/working_dir/CPU2017/510.parest_r/ref.prm", NULL},
    // 51 -> povray_r
    {"/home/malursem/working_dir/CPU2017_bin/povray_r.ppc64", "/home/malursem/working_dir/CPU2017/511.povray_r/SPEC-benchmark-ref.ini", NULL},
    // 52 -> xz_r 2
    {"/home/malursem/working_dir_test/spec_bin2017/xz_r.ppc64", "/home/malursem/working_dir_test/CPU2017/557.xz_r/cpu2006docs.tar.xz", "250", "055ce243071129412e9dd0b3b69a21654033a9b723d874b2015c774fac1553d9713be561ca86f74e4f16f22e664fc17a79f30caa5ad2c04fbc447549c2810fae", "23047774", "23513385", "6e", NULL },
    // 53 -> xz_r 3
    {"/home/malursem/working_dir_test/spec_bin2017/xz_r.ppc64", "/home/malursem/working_dir_test/CPU2017/557.xz_r/input.combined.xz", "250", "a841f68f38572a49d86226b7ff5baeb31bd19dc637a922a972b2e6d1257a890f6a544ecab967c313e370478c74f760eb229d4eef8a8d2836d233d3e9dd1430bf", "40401484", "41217675", "7", NULL },
    // 54 -> exchange2_r
    {"/home/malursem/working_dir_test/spec_bin2017/exchange2_r.ppc64","6",NULL},
    // 55 -> perlbench_r diffmail
    {"/home/malursem/working_dir_test/spec_bin2017/perlbench_r.ppc64", "-I/home/malursem/working_dir_test/CPU2017/500.perlbench_r/lib", "/home/malursem/working_dir_test/CPU2017/500.perlbench_r/diffmail.pl", "4", "800", "10", "17", "19", "300", NULL}
};

/*************************************************************
 **                   Nombre de los benchmarks              **
 *************************************************************/

char *bench_Names [] = {
	"perlbench","bzip2","gcc","mcf","gobmk","hmmer","sjeng","libquantum",//0--7
	"h264ref","omnetpp","astar","xalancbmk","bwaves","gamess","milc","zeusmp",//8--15
	"gromacs","cactusADM","leslie3d","namd","microbench","soplex","povray","GemsFDTD",//16--23
	"lbm","tonto","calculix","null","null","null","perlbench_r checkspam","gcc_r",//24--31
	"mcf_r","omnetpp_s","xalancbmk_s","x264_s","deepsjeng_r","leela_s","exchange2_s","xz_r 1",//32--39
	"bwaves_r","cactuBSSN_r","lbm_r","wrf_s","pop2_s","imagick_r","nab_s","fotonik3d_r",//40--47
	"roms_r","namd_r","parest_r","povray_r","xz_r 2","xz_r 3","exchange2_r","perlbench_r diffmail"//48--55
};

/*************************************************************
 **            Instrucciones a ejecutar por benchmark       **
 *************************************************************/

// Instrucciones para 180 segundos (3 minutios)
unsigned long int bench_Instructions [] = {
	0,840132874602,819265952766,305499573366,0,1164472528358,918983143036,720473356695,//0--7
	1203024206200,286223875335,535640883080,668801495501,618761719072,1336258416852,304422500623,754511132579,//8--15
	696156907677,1267656091383,739994193451,747682249857,76418176477,569440597876,937431955168,567490647155,//16--23
	667552291671,979918078612,1166159554490,0,0,0,878297396047,827749742594,//24--31
	452161239739,0,654338747576,1269764691582,913289873736,796433853679,0,534316783838,//32--39
	0,265381826929,630784833095,0,0,1679944953990,0,0,//40--47
	872622924668,1050380054119,863491300167,941815758997,907669994640,761005521363,0,1046945879024//48--55
};

/*************************************************************
 **                   Tamaño de la mezcla                   **
 *************************************************************/

int bench_mixes [] = { // Numero de cargas que contiene la mezcla
	1,	// 0	NO MODIFICAR aplicacion seleccionada en solitario.
	4,	// 1	NO MODIFICAR Benchmark y aplicacion seleccionada.

	// Cargas de 6 aplicaciones, aplicación de la etiqueta en core 0
	6,	// 2 gcc
	6,	// 3 mcf
	6,	// 4 bzip2 test (position modified from 1)
	6,	// 5 hmmer
	6,	// 6 sjeng test
	6,	// 7 libquantum
	6,	// 8 h264ref
	6,	// 9 omnetpp
	6,	// 10 astar test
	6,	// 11 xalancbmk
	6,	// 12 bwaves
	6,	// 13 gamess
	6,	// 14 milc
	6,	// 15 zeusmp
	6,	// 16 gromacs test
	6,	// 17 cactusADM
	6,	// 18 leslie3d
	6,	// 19 namd test
	6,	// 20 calculix test (position modified from 26)
	6,	// 21 soplex
	6,	// 22 povray test
	6,	// 23 GemsFDTD
	6,	//24 lbm
	6,	//25 tonto test

	//app 2
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	//app 3
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	//app 5
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	//app 7
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	//app 8
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	//app 9
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	//app 11
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	//app 12
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	//app 13
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	//app 14
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	//app 15
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	//app 17
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	//app 18
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	//app 21
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	//app 23
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	//app 24
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,
	6,

	// Cargas de 6 aplicaciones, aplicación de la etiqueta en core 0
	6,	// 2 gcc
	6,	// 3 mcf
	6,	// 4 bzip2 test (position modified from 1)
	6,	// 5 hmmer
	6,	// 6 sjeng test
	6,	// 7 libquantum
	6,	// 8 h264ref
	6,	// 9 omnetpp
	6,	// 10 astar test
	6,	// 11 xalancbmk
	6,	// 12 bwaves
	6,	// 13 gamess
	6,	// 14 milc
	6,	// 15 zeusmp
	6,	// 16 gromacs test
	6,	// 17 cactusADM
	6,	// 18 leslie3d
	6,	// 19 namd test
	6,	// 20 calculix test (position modified from 26)
	6,	// 21 soplex
	6,	// 22 povray test
	6,	// 23 GemsFDTD
	6,	//24 lbm
	6,	//25 tonto test
	// Cargas de 6 aplicaciones, aplicación de la etiqueta en core 0
	6,	// 2 gcc
	6,	// 3 mcf
	6,	// 4 bzip2 test (position modified from 1)
	6,	// 5 hmmer
	6,	// 6 sjeng test
	6,	// 7 libquantum
	6,	// 8 h264ref
	6,	// 9 omnetpp
	6,	// 10 astar test
	6,	// 11 xalancbmk
	6,	// 12 bwaves
	6,	// 13 gamess
	6,	// 14 milc
	6,	// 15 zeusmp
	6,	// 16 gromacs test
	6,	// 17 cactusADM
	6,	// 18 leslie3d
	6,	// 19 namd test
	6,	// 20 calculix test (position modified from 26)
	6,	// 21 soplex
	6,	// 22 povray test
	6,	// 23 GemsFDTD
	6,	//24 lbm
	6,	//25 tonto test
	// Cargas de 6 aplicaciones, aplicación de la etiqueta en core 0
	6,	// 2 gcc
	6,	// 3 mcf
	6,	// 4 bzip2 test (position modified from 1)
	6,	// 5 hmmer
	6,	// 6 sjeng test
	6,	// 7 libquantum
	6,	// 8 h264ref
	6,	// 9 omnetpp
	6,	// 10 astar test
	6,	// 11 xalancbmk
	6,	// 12 bwaves
	6,	// 13 gamess
	6,	// 14 milc
	6,	// 15 zeusmp
	6,	// 16 gromacs test
	6,	// 17 cactusADM
	6,	// 18 leslie3d
	6,	// 19 namd test
	6,	// 20 calculix test (position modified from 26)
	6,	// 21 soplex
	6,	// 22 povray test
	6,	// 23 GemsFDTD
	6,	//24 lbm
	6,	//25 tonto test
};

/*************************************************************
 **                 Composicion de la mezcla                **
 *************************************************************/

int workload_mixes [][12] = { // Cargas a ejecutar
	{-1},// 0	NO MODIFICAR aplicacion seleccionada en solitario.
	{-1,20,20,20},// 1	NO MODIFICAR Benchmark y aplicacion seleccionada.
	
	// Cargas de 6 aplicaciones inferencias test 1
	{2, 7, 6, 8, 11, 14},// 2
	{3, 23, 7, 1, 8, 23},// 3
	{1, 31, 3, 32, 7, 34},// 4 (position modified)
	{5, 10, 1, 23, 10, 22},// 5 
	{6, 9, 5, 18, 22, 2},// 6
	{7, 5, 9, 18, 11, 8},// 7
	{8, 2, 10, 15, 2, 2},// 8
	{9, 19, 11, 7, 16, 9},// 9
	{10, 2, 2, 21, 6, 9},// 10
	{11, 6, 8, 10, 21, 7},// 11
	{12, 11, 15, 18, 14, 14},// 12
	{13, 9, 36, 11, 41, 12},// 13
	{14, 37, 15, 39, 17, 41},// 14
	{15, 9, 24, 21, 18, 49},// 15
	{16, 14, 23, 3, 1, 15},// 16
	{17, 23, 35, 39, 12, 21},// 17
	{18, 8, 15, 1, 9, 22},// 18
	{19, 11, 9, 14, 18, 15},// 19
	{26, 11, 19, 12, 18, 14},// 20 (position modified)
	{21, 39, 11, 7, 37, 9},// 21
	{22, 7, 6, 42, 11, 14},// 22
	{23, 11, 15, 18, 14, 14},// 23
	{24, 2, 9, 42, 11, 33},//24
	{25, 11, 36, 22, 6, 45},//25

	//app 2
	{2, 3, 1, 5, 8, 13},//26 -> 1 app +++ en core 1
	{2, 1, 5, 8, 13, 3},//27 -> 1 app +++ en core 5
	{2, 3, 9, 14, 8, 19},//28 -> 3 app +++ en cores 1-3
	{2, 8, 19, 23, 18, 21},//29 -> 3 app +++ en cores 3-5
	{2, 3, 9, 23, 21, 14},//30 -> 5 app +++ en cores 1-5
	{2, 1, 2, 5, 8, 13},//31 -> 0 app +++ en cores 1-5
	{2, 24, 21, 7, 10, 15},//32 -> 1 app +++ en core 1
	{2, 1, 5, 8, 13, 3},//33 -> 1 app +++ en core 5
	{2, 9, 11, 12, 24, 19},//34 -> 3 app +++ en cores 1-3
	{2, 23, 24, 14, 10, 6},//35 -> 3 app +++ en cores 3-5
	//app 3
	{3, 3, 1, 5, 8, 13},//36 -> 1 app +++ en core 1
	{3, 1, 5, 8, 13, 3},//37 -> 1 app +++ en core 5
	{3, 3, 9, 14, 8, 19},//38 -> 3 app +++ en cores 1-3
	{3, 8, 19, 23, 18, 21},//39 -> 3 app +++ en cores 3-5
	{3, 3, 9, 23, 21, 14},//40 -> 5 app +++ en cores 1-5
	{3, 1, 2, 5, 8, 13},//41 -> 0 app +++ en cores 1-5
	{3, 24, 21, 7, 10, 15},//42 -> 1 app +++ en core 1
	{3, 1, 5, 8, 13, 3},//43 -> 1 app +++ en core 5
	{3, 9, 11, 12, 24, 19},//44 -> 3 app +++ en cores 1-3
	{3, 23, 24, 14, 10, 6},//45 -> 3 app +++ en cores 3-5
	//app 5
	{5, 3, 1, 5, 8, 13},//46 -> 1 app +++ en core 1
	{5, 1, 5, 8, 13, 3},//47 -> 1 app +++ en core 5
	{5, 3, 9, 14, 8, 19},//48 -> 3 app +++ en cores 1-3
	{5, 8, 19, 23, 18, 21},//49 -> 3 app +++ en cores 3-5
	{5, 3, 9, 23, 21, 14},//50 -> 5 app +++ en cores 1-5
	{5, 1, 2, 5, 8, 13},//51 -> 0 app +++ en cores 1-5
	{5, 24, 21, 7, 10, 15},//52 -> 1 app +++ en core 1
	{5, 1, 5, 8, 13, 3},//53 -> 1 app +++ en core 5
	{5, 9, 11, 12, 24, 19},//54 -> 3 app +++ en cores 1-3
	{5, 23, 24, 14, 10, 6},//55 -> 3 app +++ en cores 3-5
	//app 7
	{7, 3, 1, 5, 8, 13},//56 -> 1 app +++ en core 1
	{7, 1, 5, 8, 13, 3},//57 -> 1 app +++ en core 5
	{7, 3, 9, 14, 8, 19},//58 -> 3 app +++ en cores 1-3
	{7, 8, 19, 23, 18, 21},//59 -> 3 app +++ en cores 3-5
	{7, 3, 9, 23, 21, 14},//60 -> 5 app +++ en cores 1-5
	{7, 1, 2, 5, 8, 13},//61 -> 0 app +++ en cores 1-5
	{7, 24, 21, 7, 10, 15},//62 -> 1 app +++ en core 1
	{7, 1, 5, 8, 13, 3},//63 -> 1 app +++ en core 5
	{7, 9, 11, 12, 24, 19},//64 -> 3 app +++ en cores 1-3
	{7, 23, 24, 14, 10, 6},//65 -> 3 app +++ en cores 3-5
	//app 8
	{8, 3, 1, 5, 8, 13},//66 -> 1 app +++ en core 1
	{8, 1, 5, 8, 13, 3},//67 -> 1 app +++ en core 5
	{8, 3, 9, 14, 8, 19},//68 -> 3 app +++ en cores 1-3
	{8, 8, 19, 23, 18, 21},//69 -> 3 app +++ en cores 3-5
	{8, 3, 9, 23, 21, 14},//70 -> 5 app +++ en cores 1-5
	{8, 1, 2, 5, 8, 13},//71 -> 0 app +++ en cores 1-5
	{8, 24, 21, 7, 10, 15},//72 -> 1 app +++ en core 1
	{8, 1, 5, 8, 13, 3},//73 -> 1 app +++ en core 5
	{8, 9, 11, 12, 24, 19},//74 -> 3 app +++ en cores 1-3
	{8, 23, 24, 14, 10, 6},//75 -> 3 app +++ en cores 3-5
	//app 9
	{9, 3, 1, 5, 8, 13},//76 -> 1 app +++ en core 1
	{9, 1, 5, 8, 13, 3},//77 -> 1 app +++ en core 5
	{9, 3, 9, 14, 8, 19},//78 -> 3 app +++ en cores 1-3
	{9, 8, 19, 23, 18, 21},//79 -> 3 app +++ en cores 3-5
	{9, 3, 9, 23, 21, 14},//80 -> 5 app +++ en cores 1-5
	{9, 1, 2, 5, 8, 13},//81 -> 0 app +++ en cores 1-5
	{9, 24, 21, 7, 10, 15},//82 -> 1 app +++ en core 1
	{9, 1, 5, 8, 13, 3},//83 -> 1 app +++ en core 5
	{9, 9, 11, 12, 24, 19},//84 -> 3 app +++ en cores 1-3
	{9, 23, 24, 14, 10, 6},//85 -> 3 app +++ en cores 3-5
	//app 11
	{11, 3, 1, 5, 8, 13},//86 -> 1 app +++ en core 1
	{11, 1, 5, 8, 13, 3},//87 -> 1 app +++ en core 5
	{11, 3, 9, 14, 8, 19},//88 -> 3 app +++ en cores 1-3
	{11, 8, 19, 23, 18, 21},//89 -> 3 app +++ en cores 3-5
	{11, 3, 9, 23, 21, 14},//90 -> 5 app +++ en cores 1-5
	{11, 1, 2, 5, 8, 13},//91 -> 0 app +++ en cores 1-5
	{11, 24, 21, 7, 10, 15},//92 -> 1 app +++ en core 1
	{11, 1, 5, 8, 13, 3},//93 -> 1 app +++ en core 5
	{11, 9, 11, 12, 24, 19},//94 -> 3 app +++ en cores 1-3
	{11, 23, 24, 14, 10, 6},//95 -> 3 app +++ en cores 3-5
	//app 12
	{12, 3, 1, 5, 8, 13},//96 -> 1 app +++ en core 1
	{12, 1, 5, 8, 13, 3},//97 -> 1 app +++ en core 5
	{12, 3, 9, 14, 8, 19},//98 -> 3 app +++ en cores 1-3
	{12, 8, 19, 23, 18, 21},//99 -> 3 app +++ en cores 3-5
	{12, 3, 9, 23, 21, 14},//100 -> 5 app +++ en cores 1-5
	{12, 1, 2, 5, 8, 13},//101 -> 0 app +++ en cores 1-5
	{12, 24, 21, 7, 10, 15},//102 -> 1 app +++ en core 1
	{12, 1, 5, 8, 13, 3},//103 -> 1 app +++ en core 5
	{12, 9, 11, 12, 24, 19},//104 -> 3 app +++ en cores 1-3
	{12, 23, 24, 14, 10, 6},//105 -> 3 app +++ en cores 3-5
	//app 13
	{13, 3, 1, 5, 8, 13},//106 -> 1 app +++ en core 1
	{13, 1, 5, 8, 13, 3},//107 -> 1 app +++ en core 5
	{13, 3, 9, 14, 8, 19},//108 -> 3 app +++ en cores 1-3
	{13, 8, 19, 23, 18, 21},//109 -> 3 app +++ en cores 3-5
	{13, 3, 9, 23, 21, 14},//110 -> 5 app +++ en cores 1-5
	{13, 1, 2, 5, 8, 13},//111 -> 0 app +++ en cores 1-5
	{13, 24, 21, 7, 10, 15},//112 -> 1 app +++ en core 1
	{13, 1, 5, 8, 13, 3},//113 -> 1 app +++ en core 5
	{13, 9, 11, 12, 24, 19},//114 -> 3 app +++ en cores 1-3
	{13, 23, 24, 14, 10, 6},//115 -> 3 app +++ en cores 3-5
	//app 14
	{14, 3, 1, 5, 8, 13},//116 -> 1 app +++ en core 1
	{14, 1, 5, 8, 13, 3},//117 -> 1 app +++ en core 5
	{14, 3, 9, 14, 8, 19},//118 -> 3 app +++ en cores 1-3
	{14, 8, 19, 23, 18, 21},//119 -> 3 app +++ en cores 3-5
	{14, 3, 9, 23, 21, 14},//120 -> 5 app +++ en cores 1-5
	{14, 1, 2, 5, 8, 13},//121 -> 0 app +++ en cores 1-5
	{14, 24, 21, 7, 10, 15},//122 -> 1 app +++ en core 1
	{14, 1, 5, 8, 13, 3},//123 -> 1 app +++ en core 5
	{14, 9, 11, 12, 24, 19},//124 -> 3 app +++ en cores 1-3
	{14, 23, 24, 14, 10, 6},//125 -> 3 app +++ en cores 3-5
	//app 15
	{15, 3, 1, 5, 8, 13},//126 -> 1 app +++ en core 1
	{15, 1, 5, 8, 13, 3},//127 -> 1 app +++ en core 5
	{15, 3, 9, 14, 8, 19},//128 -> 3 app +++ en cores 1-3
	{15, 8, 19, 23, 18, 21},//129 -> 3 app +++ en cores 3-5
	{15, 3, 9, 23, 21, 14},//130 -> 5 app +++ en cores 1-5
	{15, 1, 2, 5, 8, 13},//131 -> 0 app +++ en cores 1-5
	{15, 24, 21, 7, 10, 15},//132 -> 1 app +++ en core 1
	{15, 1, 5, 8, 13, 3},//133 -> 1 app +++ en core 5
	{15, 9, 11, 12, 24, 19},//134 -> 3 app +++ en cores 1-3
	{15, 23, 24, 14, 10, 6},//135 -> 3 app +++ en cores 3-5
	//app 17
	{17, 3, 1, 5, 8, 13},//136 -> 1 app +++ en core 1
	{17, 1, 5, 8, 13, 3},//137 -> 1 app +++ en core 5
	{17, 3, 9, 14, 8, 19},//138 -> 3 app +++ en cores 1-3
	{17, 8, 19, 23, 18, 21},//139 -> 3 app +++ en cores 3-5
	{17, 3, 9, 23, 21, 14},//140 -> 5 app +++ en cores 1-5
	{17, 1, 2, 5, 8, 13},//141 -> 0 app +++ en cores 1-5
	{17, 24, 21, 7, 10, 15},//142 -> 1 app +++ en core 1
	{17, 1, 5, 8, 13, 3},//143 -> 1 app +++ en core 5
	{17, 9, 11, 12, 24, 19},//144 -> 3 app +++ en cores 1-3
	{17, 23, 24, 14, 10, 6},//145 -> 3 app +++ en cores 3-5
	//app 18
	{18, 3, 1, 5, 8, 13},//146 -> 1 app +++ en core 1
	{18, 1, 5, 8, 13, 3},//147 -> 1 app +++ en core 5
	{18, 3, 9, 14, 8, 19},//148 -> 3 app +++ en cores 1-3
	{18, 8, 19, 23, 18, 21},//149 -> 3 app +++ en cores 3-5
	{18, 3, 9, 23, 21, 14},//150 -> 5 app +++ en cores 1-5
	{18, 1, 2, 5, 8, 13},//151 -> 0 app +++ en cores 1-5
	{18, 24, 21, 7, 10, 15},//152 -> 1 app +++ en core 1
	{18, 1, 5, 8, 13, 3},//153 -> 1 app +++ en core 5
	{18, 9, 11, 12, 24, 19},//154 -> 3 app +++ en cores 1-3
	{18, 23, 24, 14, 10, 6},//155 -> 3 app +++ en cores 3-5
	//app 21
	{21, 3, 1, 5, 8, 13},//156 -> 1 app +++ en core 1
	{21, 1, 5, 8, 13, 3},//157 -> 1 app +++ en core 5
	{21, 3, 9, 14, 8, 19},//158 -> 3 app +++ en cores 1-3
	{21, 8, 19, 23, 18, 21},//159 -> 3 app +++ en cores 3-5
	{21, 3, 9, 23, 21, 14},//160 -> 5 app +++ en cores 1-5
	{21, 1, 2, 5, 8, 13},//161 -> 0 app +++ en cores 1-5
	{21, 24, 21, 7, 10, 15},//162 -> 1 app +++ en core 1
	{21, 1, 5, 8, 13, 3},//163 -> 1 app +++ en core 5
	{21, 9, 11, 12, 24, 19},//164 -> 3 app +++ en cores 1-3
	{21, 23, 24, 14, 10, 6},//165 -> 3 app +++ en cores 3-5
	//app 23
	{23, 3, 1, 5, 8, 13},//166 -> 1 app +++ en core 1
	{23, 1, 5, 8, 13, 3},//167 -> 1 app +++ en core 5
	{23, 3, 9, 14, 8, 19},//168 -> 3 app +++ en cores 1-3
	{23, 8, 19, 23, 18, 21},//169 -> 3 app +++ en cores 3-5
	{23, 3, 9, 23, 21, 14},//170 -> 5 app +++ en cores 1-5
	{23, 1, 2, 5, 8, 13},//171 -> 0 app +++ en cores 1-5
	{23, 24, 21, 7, 10, 15},//172 -> 1 app +++ en core 1
	{23, 1, 5, 8, 13, 3},//173 -> 1 app +++ en core 5
	{23, 9, 11, 12, 24, 19},//174 -> 3 app +++ en cores 1-3
	{23, 23, 24, 14, 10, 6},//175 -> 3 app +++ en cores 3-5
	//app 24
	{24, 3, 1, 5, 8, 13},//176 -> 1 app +++ en core 1
	{24, 1, 5, 8, 13, 3},//177 -> 1 app +++ en core 5
	{24, 3, 9, 14, 8, 19},//178 -> 3 app +++ en cores 1-3
	{24, 8, 19, 23, 18, 21},//179 -> 3 app +++ en cores 3-5
	{24, 3, 9, 23, 21, 14},//180 -> 5 app +++ en cores 1-5
	{24, 1, 2, 5, 8, 13},//181 -> 0 app +++ en cores 1-5
	{24, 24, 21, 7, 10, 15},//182 -> 1 app +++ en core 1
	{24, 1, 5, 8, 13, 3},//183 -> 1 app +++ en core 5
	{24, 9, 11, 12, 24, 19},//184 -> 3 app +++ en cores 1-3
	{24, 23, 24, 14, 10, 6},//185 -> 3 app +++ en cores 3-5

	// Cargas de 6 aplicaciones inferencias test 2
	//cores	0 8 16 24 32 40
	{2, 2, 23, 5, 22, 10},// 186 2 --
	{3, 19, 5, 17, 9, 14},// 187 3 --
	{1, 22, 22, 17, 11, 12},//188 4 (position modified) --
	{5, 10, 14, 11, 5, 17},//189 5 --
	{6, 9, 13, 19, 13, 23},//190 6 --
	{7, 17, 17, 2, 11, 16},// 191 7 --
	{8, 21, 24, 8, 23, 24},// 192 8 --
	{9, 13, 24, 1, 20, 15},// 193 9 --
	{10, 17, 25, 20, 21, 3},// 194 10 --
	{11, 12, 9, 24, 1, 23},// 195 11 --
	{12, 9, 19, 13, 5, 24},// 196 12 --
	{13, 16, 8, 17, 6, 23},// 197 13 --
	{14, 10, 25, 16, 24, 16},// 198 14 --
	{15, 17, 6, 17, 13, 12},// 199 15 --
	{16, 6, 9, 5, 10, 3},// 200 16 --
	{17, 24, 12, 9, 2, 22},// 201 17 --
	{18, 3, 2, 16, 20, 21},// 202 18 --
	{19, 14, 24, 20, 5, 16},// 203 19 --
	{26, 23, 3, 8, 19, 18},// 204 20 (position modified) --
	{21, 10, 6, 17, 8, 9},// 205 21 --
	{22, 5, 25, 1, 18, 9},// 206 22 --
	{23, 13, 11, 16, 22, 5},// 207 23 --
	{24, 10, 25, 12, 3, 24},// 208 24 --
	{25, 15, 16, 6, 23, 13},// 209 25 --
	// Cargas de 6 aplicaciones inferencias test 3
	{2, 15, 13, 1, 3, 11},// 210 2 --
	{3, 1, 1, 22, 1, 3},// 211 3 --
	{1, 1, 18, 15, 7, 12},// 212 4 (position modified) --
	{5, 21, 11, 18, 15, 17},// 213 5 --
	{6, 13, 12, 6, 22, 8},// 214 6 --
	{7, 11, 17, 16, 10, 3},// 215 7 --
	{8, 20, 13, 14, 9, 23},// 216 8 --
	{9, 11, 13, 7, 14, 11},// 217 9 --
	{10, 1, 11, 21, 14, 22},// 218 10 --
	{11, 7, 7, 13, 19, 16},// 219 11 --
	{12, 18, 3, 25, 24, 21},// 220 12 --
	{13, 18, 17, 1, 14, 6},// 221 13 --
	{14, 14, 25, 1, 14, 10},// 222 14 --
	{15, 17, 12, 21, 5, 6},// 223 15 --
	{16, 17, 14, 24, 18, 24},// 224 16 --
	{17, 2, 1, 11, 23, 15},// 225 17 --
	{18, 3, 14, 11, 24, 19},// 226 18 --
	{19, 17, 8, 8, 1, 10},// 227 19 --
	{26, 5, 12, 9, 13, 20},// 228 20 (position modified) --
	{21, 21, 8, 21, 1, 9},// 229 21 --
	{22, 2, 23, 17, 16, 18},// 230 22 --
	{23, 17, 25, 23, 10, 3},// 231 23 --
	{24, 3, 9, 14, 13, 14},// 232 24 --
	{25, 25, 17, 20, 14, 1},// 233 25 --
	// Cargas de 6 aplicaciones inferencias test 4
	{2, 2, 25, 3, 8, 17},// 234 2 --
	{3, 24, 11, 1, 18, 19},// 235 3 --
	{1, 9, 14, 16, 11, 6},// 236 4 (position modified) --
	{5, 8, 23, 13, 16, 1},// 237 5 --
	{6, 1, 24, 11, 11, 20},// 238 6 --
	{7, 8, 12, 2, 8, 13},// 239 7 --
	{8, 1, 21, 8, 23, 2},// 240 8 --
	{9, 2, 21, 22, 22, 12},// 241 9 --
	{10, 10, 7, 3, 14, 3},// 242 10 --
	{11, 14, 6, 24, 6, 21},// 243 11 --
	{12, 16, 21, 14, 20, 12},// 244 12 --
	{13, 9, 23, 14, 15, 15},// 245 13 --
	{14, 7, 9, 16, 20, 3},// 246 14 --
	{15, 20, 22, 8, 3, 9},// 247 15 --
	{16, 14, 11, 25, 15, 17},// 248 16 --
	{17, 11, 1, 7, 25, 22},// 249 17 --
	{18, 22, 5, 23, 6, 21},// 250 18 --
	{19, 12, 10, 9, 6, 23},// 251 19 --
	{26, 8, 3, 18, 24, 9},// 252 20 (position modified) --
	{21, 25, 7, 3, 14, 12},// 253 21 --
	{22, 10, 22, 11, 13, 10},// 254 22 --
	{23, 25, 6, 1, 20, 22},// 255 23 --
	{24, 1, 25, 9, 7, 24},// 256 24 --
	{25, 5, 15, 3, 2, 24},// 257 25 --
};

/*************************************************************
 **                 do_dscr_pid                             **
 *************************************************************/

static int do_dscr_pid(int dscr_state, pid_t pid){
	int rc;
	
	//fprintf(stdout, "INFO: The process %d set the DSCR value to %d.\n",pid,dscr_state);
	rc = ptrace(PTRACE_ATTACH, pid, NULL, NULL);
	if(rc) {
		fprintf(stderr, "ERROR: Could not attach to process %d to %s the DSCR value\n%s\n", pid,(dscr_state ? "set" : "get"), strerror(errno));
		return rc;
	}
	wait(NULL);
	rc = ptrace(PTRACE_POKEUSER, pid, PTRACE_DSCR << 3, dscr_state);
	if(rc) {
		fprintf(stderr, "ERROR: Could not set the DSCR value for pid %d\n%s\n", pid, strerror(errno));
		ptrace(PTRACE_DETACH, pid, NULL, NULL);
		return rc;
	}
	rc = ptrace(PTRACE_PEEKUSER, pid, PTRACE_DSCR << 3, NULL);
	if (errno) {
		fprintf(stderr, "ERROR: Could not get the DSCR value for pid %d\n%s\n", pid, strerror(errno));
		rc = -1;
	}/* else {
		fprintf(stdout, "INFO: DSCR for pid %d is %d\n", pid, rc);
	}*/
	ptrace(PTRACE_DETACH, pid, NULL, NULL);
	return rc;
}

/*************************************************************
 **                 initialize_events                       **
 *************************************************************/

void initialize_events(node *node) {
	int i, ret;

	// Configure events
	ret = perf_setup_list_events(options.events, &(node->fds), &(node->num_fds));
	if(ret || (node->num_fds == 0)) {
		exit(1);
	}
	node->fds[0].fd = -1;
	for(i=0; i<node->num_fds; i++) {
		node->fds[i].hw.disabled = 0;  /* start immediately */
		/* request timing information necessary for scaling counts */
		node->fds[i].hw.read_format = PERF_FORMAT_SCALE;
		node->fds[i].hw.pinned = !i && options.pinned;
		node->fds[i].fd = perf_event_open(&node->fds[i].hw, node->pid, -1, (options.group? node->fds[i].fd : -1), 0);
		if(node->fds[i].fd == -1) {
			fprintf(stderr, "ERROR: cannot attach event %s\n", node->fds[i].name);
		}
	}
}

/*************************************************************
 **                 finalize_events                         **
 *************************************************************/

void finalize_events(node *node) {
  int i;

  // Releases descriptors
  for(i=0; i < node->num_fds; i++) {
    close(node->fds[i].fd);
  }
  // Release the counters
  perf_free_fds(node->fds, node->num_fds);
  node->fds = NULL;
}

/*************************************************************
 **                 initialize_counters                     **
 *************************************************************/

void initialize_counters(node *node) {
  int i;
  for(i=0; i<45; i++) {
    node->counters[i] = 0;
  }
}

/*************************************************************
 **                 initialize_my_Counters                  **
 *************************************************************/

void initialize_my_Counters(node *node) {
	int j;
	for(j=0;j<num_Counters;j++){
		node->my_Counters[j] = 0;
	}
}

/*************************************************************
 **                  get_counts                             **
 *************************************************************/

static void get_counts(node *aux){
  ssize_t ret;
  int i,cont = 0;
	for(i=0; i < aux->num_fds; i++) {
		uint64_t val;
		ret = read(aux->fds[i].fd, aux->fds[i].values, sizeof(aux->fds[i].values));
		if(ret < (ssize_t)sizeof(aux->fds[i].values)) {
			if(ret == -1)
				fprintf(stderr, "ERROR: cannot read values event %s\n", aux->fds[i].name);
			else
				fprintf(stderr,"ERROR: could not read event %d\n", i);
		}
		val = aux->fds[i].values[0] - aux->fds[i].prev_values[0];
		aux->fds[i].prev_values[0] = aux->fds[i].values[0];
		aux->counters[i] += val;// Save all acumulated counters during the execution
		switch(i) {
			case 0:// Cycles
				aux->a_cycles = val;
				aux->total_cycles += val;
				//fprintf(stdout, "CASE 0: PID = %d, Cycles = %ld\n",aux->pid,aux->a_cycles);
				break;
			case 1:// Instructions
				aux->a_instructions = val;
				aux->total_instructions += val;
				break;
			default:// Others
				aux->my_Counters[cont] = val;
				cont++;
				break;
		}
	}
}

/*************************************************************
 **                 launch_process                          **
 *************************************************************/

int launch_process(node *node) {
	FILE *fitxer;
	pid_t pid;
	node->relauched++;
	/*if(node->relauched > 0){
		core_Info = fopen(node->core_out, "a");
		fprintf(core_Info,"Application %d relaunched %d times.\n",node->benchmark, node->relauched);
		fclose(core_Info);
	}*/
	pid = fork();
	switch(pid) {
		case -1: // ERROR
			fprintf(stderr, "ERROR: Couldn't create the child.\n");
			exit(-3);

		case 0: // Child
        	// Descriptors for those who have input for standard input
        	switch(node->benchmark) {
				case 4: // [Doesn't work]
					close(0);
					fitxer = fopen("/home/malursem/working_dir_test/CPU2006/445.gobmk/data/ref/input/13x13.tst", "r");
					if(fitxer == NULL) {
					fprintf(stderr,"ERROR: The file could not be opened: working_dir/CPU2006/445.gobmk/data/ref/input/13x13.tst.\n");
					return -1;
					}
					break;

				case 9:
					system("cp /home/malursem/working_dir_test/omnetpp_2006.ini /home/malursem/working_dir_test/omnetpp.ini  >/dev/null 2>&1");
					break;

				case 13:
					close(0);
					fitxer = fopen("/home/malursem/working_dir/CPU2006/416.gamess/data/ref/input/h2ocu2+.gradient.config", "r");
					if(fitxer == NULL) {
					fprintf(stderr,"ERROR: The file could not be opened: working_dir/CPU2006/416.gamess/data/ref/input/h2ocu2+.gradient.config.\n");
					return -1;
					}
					break;

				case 14:
					close(0);
					fitxer = fopen("/home/malursem/working_dir/CPU2006/433.milc/data/ref/input/su3imp.in", "r");
					if(fitxer == NULL) {
					fprintf(stderr,"ERROR: The file could not be opened: working_dir/CPU2006/433.milc/data/ref/input/su3imp.in.\n");
					return -1;
					}
					break;

				case 18:
					close(0);
					fitxer = fopen("/home/malursem/working_dir/CPU2006/437.leslie3d/data/ref/input/leslie3d.in", "r");
					if(fitxer == NULL) {
					fprintf(stderr,"ERROR: The file could not be opened: working_dir/CPU2006/437.leslie3d/data/ref/input/leslie3d.in.\n");
					return -1;
					}
					break;

				case 22:
					close(2);
					fitxer = fopen("/home/malursem/working_dir/povray.sal", "w");
					if(fitxer == NULL) {
					fprintf(stderr,"ERROR: The file could not be opened: working_dir/povray.sal\n");
					return -1;
					}
					break;

				case 34:
					close(0);
					fitxer = fopen("/home/malursem/working_dir_test/CPU2017/503.bwaves_r/bwaves_1.in", "r");
					if(fitxer == NULL) {
					fprintf(stderr,"ERROR: The file could not be opened: working_dir_test/CPU2017/503.bwaves_r/bwaves_1.in.\n");
					return -1;
					}
					break;

				case 41:
					system("cp /home/malursem/working_dir_test/omnetpp_2017.ini /home/malursem/working_dir_test/omnetpp.ini  >/dev/null 2>&1");
					break;

				case 54:
					close(0);
					fitxer = fopen("/home/malursem/working_dir_test/CPU2017/554.roms_r/ocean_benchmark2.in.x", "r");
					if(fitxer == NULL) {
					fprintf(stderr,"ERROR: The file could not be opened: working_dir_test/CPU2017/554.roms_r/ocean_benchmark2.in.x.\n");
					return -1;
					}
					break;
    		}
			execv(benchmarks[node->benchmark][0], benchmarks[node->benchmark]);
			fprintf(stderr, "ERROR: Couldn't launch the program %d.\n",node->benchmark);
			exit(-2);

		default:// Parent
			usleep(100); // Wait
			// We pause the process
			kill(pid, 19);
			// We see that it has not failed
			waitpid(pid, &(node->status), WUNTRACED);
			if(WIFEXITED(node->status)) {
				fprintf(stderr, "ERROR: command process %d exited too early with status %d\n", pid, WEXITSTATUS(node->status));
				return -2;
			}
			// The pid is assigned
			node->pid = pid;
			if(sched_setaffinity(node->pid, sizeof(node->mask), &node->mask) != 0) {
				fprintf(stderr,"ERROR: Sched_setaffinity %d.\n", errno);
				exit(1);
			}
			// Put the prefetcher configuration value of the process
			do_dscr_pid(node->actual_DSCR,node->pid);
			
			return 1;
	}
}

/************************************************************
**                  PRINT DATA FROM CORE	               **
*************************************************************/

void printData(node *node) {
	int j;
	/*
		SAVE THE OUTPUT FOR EACH VARIABLE
	*/
	core_Info = fopen(node->core_out, "a");
	node->sum_BW = sum_BW - node->actual_BW;// Get the BW consumed by others
	if(!(node->finished)){// Since you want to store only the target instructions, you must put this if so that it stops saving the results of the ending core.
		fprintf(core_Info,"%"PRIu64"\t%"PRIu64"\t%"PRIu64"\t%"PRIu64"\t%f\t%f\t%f\t%f",
		node->a_instructions,node->a_cycles,node->total_instructions,node->total_cycles,node->sum_BW,node->actual_BW,node->actual_IPC,node->predicted_IPC);
	}
	/*
		PRINT VALUE FOR THE CURRENT COUNTERS.
	*/
	if(!(node->finished)){// Since you want to store only the target instructions, you must put this if so that it stops saving the results of the ending core.	
	for(j=0; j < num_Counters; j++) {
			if(j==num_Counters-1){
		 		fprintf(core_Info, "\t%"PRIu64"\n", node->my_Counters[j]);
		 	}else{
		 		fprintf(core_Info,"\t%"PRIu64"", node->my_Counters[j]);
		 	}
		}
	}
	fclose(core_Info);
}

/************************************************************
**                  INICIO DE LA PREDICCION                **
*************************************************************/

int predecirIPC(node *node) {
	fprintf(stdout, "INFO: Launching process PID=%d 1 quantum.\n",node->pid);
	int ret = 0;
	// Free processes (SIGCONT-Continue a paused process)
	if(node->pid > 0) {
		kill(node->pid, 18);
		//fprintf(stdout, "INFO: Process %d runing with status %d\n", node->pid, WEXITSTATUS(node->status));
	}
	// Check that everyone has performed the operation
	waitpid(node->pid, &(node->status), WCONTINUED);
	if(WIFEXITED(node->status)) {
		fprintf(stdout, "ERROR: command process %d_%d exited too early with status %d\n", node->benchmark, node->pid, WEXITSTATUS(node->status));
	}
	// We run for a quantum
	usleep(options.delay*1000);
	// Pause processes (SIGSTOP-Pause a process)
	if(node->pid > 0) {
		kill(node->pid, 19);
		//fprintf(stdout, "INFO: Process %d paused with status %d\n", node->pid, WEXITSTATUS(node->status));
	}
	// Check that no process has died
	waitpid(node->pid, &(node->status), WUNTRACED);
	if(WIFEXITED(node->status)) {
		//fprintf(stdout, "INFO: Process %d finished with status %d\n", node->pid, WEXITSTATUS(node->status));
		ret++;
		node->pid = -1;
	}
	
	get_counts(node);
		
	if(node->a_cycles != 0){
		node->predicted_IPC =  (float) node->a_instructions / (float) node->a_cycles;
	}else{
		node->predicted_IPC = 0.0;
	}
	fprintf(stdout, "INFO: Prediction for the next quanta is %f for the process PID=%d.\n",node->predicted_IPC,node->pid);

	return ret;
}

/*************************************************************
 **                 measure                                 **
 *************************************************************/

int measure() {
	int i, ret = 0;
	// Free processes (SIGCONT-Continue a paused process)
	for(i=0; i<N; i++) {
		if(queue[i].pid > 0) {
			kill(queue[i].pid, 18);
			//fprintf(stdout, "INFO: Process %d runing with status %d\n", queue[i].pid, WEXITSTATUS(queue[i].status));
		}
	}
	// Check that everyone has performed the operation
	for(i=0; i<N; i++) {
		waitpid(queue[i].pid, &(queue[i].status), WCONTINUED);
		if(WIFEXITED(queue[i].status)) {
			fprintf(stdout, "ERROR: command process %d_%d exited too early with status %d\n", queue[i].benchmark, queue[i].pid, WEXITSTATUS(queue[i].status));
		}
	}
	// We run for a quantum
	usleep(options.delay*1000);
	// Pause processes (SIGSTOP-Pause a process)
	for(i=0; i<N; i++) {
		if(queue[i].pid > 0) {
			kill(queue[i].pid, 19);
			//fprintf(stdout, "INFO: Process %d paused with status %d\n", queue[i].pid, WEXITSTATUS(queue[i].status));
		}
	}
	// Check that no process has died
	for(i=0; i<N; i++) {
		waitpid(queue[i].pid, &(queue[i].status), WUNTRACED);
		if(WIFEXITED(queue[i].status)) {
			//fprintf(stdout, "INFO: Process %d finished with status %d\n", queue[i].pid, WEXITSTATUS(queue[i].status));
			ret++;
			queue[i].pid = -1;
		}
	}
	// Pick up counters for each process
	sum_BW = 0.0;	
	for(i=0; i<N; i++) {
		get_counts(&(queue[i]));
		/*
			UPDATE THE IPC OF PREVIOUS INTERVALS
		*/
		queue[i].last_IPC_3 = queue[i].last_IPC_2;
		queue[i].last_IPC_2 = queue[i].last_IPC_1;
		queue[i].last_IPC_1 = queue[i].actual_IPC;
		/*
			CALCULATE THE CURRENT IPC OF THE INTERVAL
		*/
		if(queue[i].a_cycles != 0){
			queue[i].actual_IPC =  (float) queue[i].a_instructions / (float) queue[i].a_cycles;
		}else{
			queue[i].actual_IPC = 0.0;
		}
		/*
			UPDATE THE BW OF PREVIOUS INTERVALS
		*/	
		queue[i].last_BW_3 = queue[i].last_BW_2;
		queue[i].last_BW_2 = queue[i].last_BW_1;
		queue[i].last_BW_1 = queue[i].actual_BW;
		/*
			CALCULATE CURRENT BANDWIDTH OF INTERVAL
		*/
		if(queue[i].a_cycles != 0){
			queue[i].actual_BW = (float) ((float) (queue[i].my_Counters[PM_MEM_PREF]+queue[i].my_Counters[LLC_LOAD_MISSES])*CPU_FREQ)/ (float) queue[i].a_cycles;
		}else{
			queue[i].actual_BW = 0.0;
		}
		sum_BW += queue[i].actual_BW;
	}		
	// Print Data for each application on each core
	for(i=0; i<N; i++){
		printData(&(queue[i]));
	}
	return ret;
}

/*************************************************************
 **                 Usage                                   **
 *************************************************************/

static void usage(void) {
  printf("\nUsage: Progress-Aware-Scheduler implementation\n\n");
  printf("[-o output_directory for the output of the prediction function (it will be divided into files for each core)]\n");
  printf("[-h [help]]\n");
  printf("-A workload [0-> Single, 1-> Single with 3 instances of the microbenchmark]\n");
  printf("[-B benchmark [Only if -A is 1 or 0]]\n");
  printf("[-C configuracionPrefetcher [Only if -A is 1 or 0] ]\n");
  printf("[-S Stride [Stride that is used in the microbenchmark, it is necessary to stipulate if it is used. Recommended value is 256]]\n");
  printf("[-N Nops [Number of nops that the microbenchmark executes, to regulate the load.]]\n");
}

/*************************************************************
 **                     MAIN PROGRAM                        **
 *************************************************************/

int main(int argc, char **argv) {

	printf("\nProgress-Aware-Scheduler implementation. Year: 2021 Author: Manel Lurbe Sempere <malursem@gap.upv.es>.\n");

	int c, i,j, ret, quantums = 0;
	int individualBench = -1;
	int individualDSCR = 0;// Default configuration. DEF
	// Set initial events to measure
	options.events=strdup("cycles,instructions,LLC-LOAD-MISSES,PM_MEM_PREF");
	options.delay = 200;
	end_experiment = 0;
	N = -1;

	while((c=getopt(argc, argv,"o:hA:B:C:PgS:N:")) != -1) {
		switch(c) {
			case 'o':
				options.output_directory = strdup(optarg);
				break;
			case 'h':
				usage();
				exit(0);
			case 'A':
				workload = atoi(optarg);
				N = bench_mixes[workload];
				break;
			/* For Single-core application only*/	
			case 'B'://Selected benchmark.
				individualBench = atoi(optarg);
				break;
			case 'C'://Selected DSCR.
				individualDSCR = atoi(optarg);
				break;
			/* libpfm options */
			case 'P':
				options.pinned = 1;
				break;
			case 'g':
				options.group = 1;
				break;
			/* Microbenchmark options */
			case 'S':// Microbenchmark Stride
				benchmarks[20][3] = optarg;
				break;
			case 'N':// Nop
				benchmarks[20][2] = optarg;
				break;
			default:
				fprintf(stderr, "Unknown error\n");
		}
	}

	for(i=0; i<N_MAX; i++) {
		/*
			Application status
		*/
		queue[i].benchmark = -1;
		queue[i].finished = 0;
		queue[i].pid = -1;
		queue[i].core = -1;
		/*
			Array where to store the counters to be measured
		*/
		queue[i].my_Counters = (uint64_t *) malloc(num_Counters * sizeof(uint64_t));
		initialize_my_Counters(&(queue[i]));
		/*
			Accumulated for each application
		*/
		queue[i].total_instructions = 0;
		queue[i].total_cycles = 0;
		/*
			From the current quantum of the application
		*/
		queue[i].a_instructions = 0;
		queue[i].a_cycles = 0;
		/*
			Values to store
		*/
		queue[i].actual_DSCR = individualDSCR;
		queue[i].actual_IPC = 0;
		queue[i].predicted_IPC = 0;
		queue[i].actual_BW = 0;
		queue[i].sum_BW = 0.0;

		// For the previous intervals we keep the IPC reached.
		queue[i].last_IPC_1 = 0;
		queue[i].last_IPC_2 = 0;
		queue[i].last_IPC_3 = 0;

		// For the previous intervals we save the consumed BW
		queue[i].last_BW_1 = 0;
		queue[i].last_BW_2 = 0;
		queue[i].last_BW_3 = 0;

		queue[i].relauched = -1;
	}
	// Select predefined cores for N_MAX
	queue[0].core = 0;
	queue[1].core = 8;
	queue[2].core = 16;
	queue[3].core = 24;
	queue[4].core = 32;
	queue[5].core = 40;
	queue[6].core = 48;
	queue[7].core = 56;
	queue[8].core = 64;
	queue[9].core = 72;

	if(N < 0) {
		fprintf(stderr, "ERROR: Number of processes not specified.\n");
		return -1;
	}
	if(!options.output_directory){
		fprintf(stderr, "ERROR: The output directory was not specified.\n");
		return -1;
	}
	for(i=0; i<N; i++) {
		queue[i].benchmark = workload_mixes[workload][i];
		queue[i].core_out = malloc(sizeof(char) * 1024);
		snprintf(queue[i].core_out, sizeof(char) * 1024, "%s%s%d%s", options.output_directory, "[", queue[i].core, "].txt");
	}
	// It looks if the load has been put 0 (Only one application) or 1 (application next to the microbenchmark)
	if(workload==0 || workload==1){
		if(individualBench<0 || individualDSCR<0){
			fprintf(stderr, "ERROR: No application or prefetch configuration specified.\n");
			return -1;
		}
		queue[0].benchmark = individualBench;
		queue[0].actual_DSCR = individualDSCR;
	}
	// See if any benchmark to assign or core is missing
	for(i=0; i<N; i++) {
		if(queue[i].benchmark < 0) {
			fprintf(stderr, "ERROR: Some process to assign benchmark is missing.\n");
			return -1;
		}
		if(queue[i].core < 0) {
			fprintf(stderr, "ERROR: Some core to assign.\n");
			return -1;
		}
	}
	// Start counters
	for(i=0; i<N; i++) {
		initialize_counters(&(queue[i]));
	}
	// Assign cores
	for(i=0; i<N; i++) {
		/*
			Each core prepares its results file, to later in the post-processing know what core the data is.
		*/
		core_Info = fopen(queue[i].core_out, "w");
		fprintf(core_Info,"instructions\tcycles\ttotal_instructions\ttotal_cycles\tsum_BW\tactual_bw\tactual_ipc\tpredict_ipc");
		for(j=0; j < num_Counters; j++) {
			if(j==num_Counters-1) {
				fprintf(core_Info, "\tCounter%d\n",j);
			} else{
				fprintf(core_Info,"\tCounter%d",j);
			}
		}
		fclose(core_Info);
		CPU_ZERO(&(queue[i].mask));	
		CPU_SET(queue[i].core, &(queue[i].mask));
	}
	// Initialize libpfm
	if(pfm_initialize() != PFM_SUCCESS) {
		fprintf(stderr,"ERROR: libpfm initialization failed\n");
	}
	for(i=0; i<N; i++) {
		launch_process(&(queue[i]));
		initialize_events(&(queue[i]));
	}
	do {
		if(planificar == 1){// Single core execution to predict the IPC for the core 0
			ret = predecirIPC(&(queue[0]));
			planificar = 0;
			if(ret) {
				if(queue[0].pid == -1) {
					// The counters are read before finalizing it
					get_counts(&(queue[0]));
					finalize_events(&(queue[0]));
					// If the instructions to be executed have been completed
					if(queue[0].total_instructions < bench_Instructions[queue[0].benchmark]){
						launch_process(&(queue[0]));
						initialize_events(&(queue[0]));
					}
				}
			}
			if(queue[0].total_instructions >= bench_Instructions[queue[0].benchmark]){
				// If you have not finished any time yet
				if(!queue[0].finished){
					end_experiment++;
					queue[0].finished = 1;
				}
				// If you are alive kill because you have passed the instructions of the experiment
				if(queue[0].pid != -1){
					kill(queue[0].pid, 9);
					queue[0].pid = -1;
					finalize_events(&(queue[0]));
				}
				queue[0].total_instructions = 0;
				// Should not print on the screen
				launch_process(&(queue[0]));
				initialize_events(&(queue[0]));
			}
		} else {// Multi core execution to run the wk normally
			ret = measure();
			// Calculate the virtual quantum of the next iteration
			if(time_to_predict < virtual_Quantums-1){//time to next prediction in quantums
				time_to_predict++;
			}else{
				planificar = 1;
				time_to_predict = 0;
			}
			// Run a quantum and collect the values of that quantum
			quantums++;
			// If any process has ended
			if(ret) {
				// Look at which of the applications has ended
				for(i=0; i<N; i++) {
					if(queue[i].pid == -1) {
						// The counters are read before finalizing it
						get_counts(&(queue[i]));
						finalize_events(&(queue[i]));
						// If the instructions to be executed have been completed
						if(queue[i].total_instructions < bench_Instructions[queue[i].benchmark]){
							launch_process(&(queue[i]));
							initialize_events(&(queue[i]));
						}
					}
				}
			}
			for(i=0; i<N; i++) {
				if(queue[i].total_instructions >= bench_Instructions[queue[i].benchmark]){
					// If you have not finished any time yet
					if(!queue[i].finished){
						end_experiment++;
						queue[i].finished = 1;
					}
					// If you are alive kill because you have passed the instructions of the experiment
					if(queue[i].pid != -1){
						kill(queue[i].pid, 9);
						queue[i].pid = -1;
						finalize_events(&(queue[i]));
					}
					queue[i].total_instructions = 0;
					// Should not print on the screen
					launch_process(&(queue[i]));
					initialize_events(&(queue[i]));
				}
			}
		}
	}while(end_experiment < N);

	// End any process that may be pending
	for(i=0; i<N; i++) {
		if(queue[i].pid > 0) {
			kill(queue[i].pid, 9);
			finalize_events(&(queue[i]));
		}
	}
	//Free libpfm resources cleanly
	pfm_terminate();
	for(i=0; i<N_MAX; i++) {
		free(queue[i].core_out);
		free(queue[i].my_Counters);
	}
	return 0;
}
