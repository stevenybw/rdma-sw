#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include <mpi.h>
#include "ympi.h"
#include "common.h"

#define ARCH_SW

#define LOCAL_BITS (20)
#define LOCAL_SIZE (1LL << LOCAL_BITS)

#define MAX_NPROCS     (4*40*1024)
#define MAX_NUM_BOARD  (256)
#define MAX_PA         (4*51200)
#define MAX_NUM_ITER   20
#define MAX_GROUP_SIZE 6

/* valid_number is computed in a python script */
static int valid_factor[] = {2,3,5};
typedef struct Factor {
  int n0;
  int n1;
  int n2;
} Factor;
#define FACTOR_SUM(FACTOR) (FACTOR.n0 + FACTOR.n1 + FACTOR.n2)
typedef struct Factorized {
  int number;
  Factor factor;
} Factorized;
static Factorized factorized[] = {{2,{1,0,0}},{3,{0,1,0}},{4,{2,0,0}},{5,{0,0,1}},{6,{1,1,0}},{8,{3,0,0}},{9,{0,2,0}},{10,{1,0,1}},{12,{2,1,0}},{15,{0,1,1}},{16,{4,0,0}},{18,{1,2,0}},{20,{2,0,1}},{24,{3,1,0}},{25,{0,0,2}},{27,{0,3,0}},{30,{1,1,1}},{32,{5,0,0}},{36,{2,2,0}},{40,{3,0,1}},{45,{0,2,1}},{48,{4,1,0}},{50,{1,0,2}},{54,{1,3,0}},{60,{2,1,1}},{64,{6,0,0}},{72,{3,2,0}},{75,{0,1,2}},{80,{4,0,1}},{81,{0,4,0}},{90,{1,2,1}},{96,{5,1,0}},{100,{2,0,2}},{108,{2,3,0}},{120,{3,1,1}},{125,{0,0,3}},{128,{7,0,0}},{135,{0,3,1}},{144,{4,2,0}},{150,{1,1,2}},{160,{5,0,1}},{162,{1,4,0}},{180,{2,2,1}},{192,{6,1,0}},{200,{3,0,2}},{216,{3,3,0}},{225,{0,2,2}},{240,{4,1,1}},{243,{0,5,0}},{250,{1,0,3}},{256,{8,0,0}},{270,{1,3,1}},{288,{5,2,0}},{300,{2,1,2}},{320,{6,0,1}},{324,{2,4,0}},{360,{3,2,1}},{375,{0,1,3}},{384,{7,1,0}},{400,{4,0,2}},{405,{0,4,1}},{432,{4,3,0}},{450,{1,2,2}},{480,{5,1,1}},{486,{1,5,0}},{500,{2,0,3}},{512,{9,0,0}},{540,{2,3,1}},{576,{6,2,0}},{600,{3,1,2}},{625,{0,0,4}},{640,{7,0,1}},{648,{3,4,0}},{675,{0,3,2}},{720,{4,2,1}},{729,{0,6,0}},{750,{1,1,3}},{768,{8,1,0}},{800,{5,0,2}},{810,{1,4,1}},{864,{5,3,0}},{900,{2,2,2}},{960,{6,1,1}},{972,{2,5,0}},{1000,{3,0,3}},{1024,{10,0,0}},{1080,{3,3,1}},{1125,{0,2,3}},{1152,{7,2,0}},{1200,{4,1,2}},{1215,{0,5,1}},{1250,{1,0,4}},{1280,{8,0,1}},{1296,{4,4,0}},{1350,{1,3,2}},{1440,{5,2,1}},{1458,{1,6,0}},{1500,{2,1,3}},{1536,{9,1,0}},{1600,{6,0,2}},{1620,{2,4,1}},{1728,{6,3,0}},{1800,{3,2,2}},{1875,{0,1,4}},{1920,{7,1,1}},{1944,{3,5,0}},{2000,{4,0,3}},{2025,{0,4,2}},{2048,{11,0,0}},{2160,{4,3,1}},{2187,{0,7,0}},{2250,{1,2,3}},{2304,{8,2,0}},{2400,{5,1,2}},{2430,{1,5,1}},{2500,{2,0,4}},{2560,{9,0,1}},{2592,{5,4,0}},{2700,{2,3,2}},{2880,{6,2,1}},{2916,{2,6,0}},{3000,{3,1,3}},{3072,{10,1,0}},{3125,{0,0,5}},{3200,{7,0,2}},{3240,{3,4,1}},{3375,{0,3,3}},{3456,{7,3,0}},{3600,{4,2,2}},{3645,{0,6,1}},{3750,{1,1,4}},{3840,{8,1,1}},{3888,{4,5,0}},{4000,{5,0,3}},{4050,{1,4,2}},{4096,{12,0,0}},{4320,{5,3,1}},{4374,{1,7,0}},{4500,{2,2,3}},{4608,{9,2,0}},{4800,{6,1,2}},{4860,{2,5,1}},{5000,{3,0,4}},{5120,{10,0,1}},{5184,{6,4,0}},{5400,{3,3,2}},{5625,{0,2,4}},{5760,{7,2,1}},{5832,{3,6,0}},{6000,{4,1,3}},{6075,{0,5,2}},{6144,{11,1,0}},{6250,{1,0,5}},{6400,{8,0,2}},{6480,{4,4,1}},{6561,{0,8,0}},{6750,{1,3,3}},{6912,{8,3,0}},{7200,{5,2,2}},{7290,{1,6,1}},{7500,{2,1,4}},{7680,{9,1,1}},{7776,{5,5,0}},{8000,{6,0,3}},{8100,{2,4,2}},{8192,{13,0,0}},{8640,{6,3,1}},{8748,{2,7,0}},{9000,{3,2,3}},{9216,{10,2,0}},{9375,{0,1,5}},{9600,{7,1,2}},{9720,{3,5,1}},{10000,{4,0,4}},{10125,{0,4,3}},{10240,{11,0,1}},{10368,{7,4,0}},{10800,{4,3,2}},{10935,{0,7,1}},{11250,{1,2,4}},{11520,{8,2,1}},{11664,{4,6,0}},{12000,{5,1,3}},{12150,{1,5,2}},{12288,{12,1,0}},{12500,{2,0,5}},{12800,{9,0,2}},{12960,{5,4,1}},{13122,{1,8,0}},{13500,{2,3,3}},{13824,{9,3,0}},{14400,{6,2,2}},{14580,{2,6,1}},{15000,{3,1,4}},{15360,{10,1,1}},{15552,{6,5,0}},{15625,{0,0,6}},{16000,{7,0,3}},{16200,{3,4,2}},{16384,{14,0,0}},{16875,{0,3,4}},{17280,{7,3,1}},{17496,{3,7,0}},{18000,{4,2,3}},{18225,{0,6,2}},{18432,{11,2,0}},{18750,{1,1,5}},{19200,{8,1,2}},{19440,{4,5,1}},{19683,{0,9,0}},{20000,{5,0,4}},{20250,{1,4,3}},{20480,{12,0,1}},{20736,{8,4,0}},{21600,{5,3,2}},{21870,{1,7,1}},{22500,{2,2,4}},{23040,{9,2,1}},{23328,{5,6,0}},{24000,{6,1,3}},{24300,{2,5,2}},{24576,{13,1,0}},{25000,{3,0,5}},{25600,{10,0,2}},{25920,{6,4,1}},{26244,{2,8,0}},{27000,{3,3,3}},{27648,{10,3,0}},{28125,{0,2,5}},{28800,{7,2,2}},{29160,{3,6,1}},{30000,{4,1,4}},{30375,{0,5,3}},{30720,{11,1,1}},{31104,{7,5,0}},{31250,{1,0,6}},{32000,{8,0,3}},{32400,{4,4,2}},{32768,{15,0,0}},{32805,{0,8,1}},{33750,{1,3,4}},{34560,{8,3,1}},{34992,{4,7,0}},{36000,{5,2,3}},{36450,{1,6,2}},{36864,{12,2,0}},{37500,{2,1,5}},{38400,{9,1,2}},{38880,{5,5,1}},{39366,{1,9,0}},{40000,{6,0,4}},{40500,{2,4,3}},{40960,{13,0,1}},{41472,{9,4,0}},{43200,{6,3,2}},{43740,{2,7,1}},{45000,{3,2,4}},{46080,{10,2,1}},{46656,{6,6,0}},{46875,{0,1,6}},{48000,{7,1,3}},{48600,{3,5,2}},{49152,{14,1,0}},{50000,{4,0,5}},{50625,{0,4,4}},{51200,{11,0,2}},{51840,{7,4,1}},{52488,{3,8,0}},{54000,{4,3,3}},{54675,{0,7,2}},{55296,{11,3,0}},{56250,{1,2,5}},{57600,{8,2,2}},{58320,{4,6,1}},{59049,{0,10,0}},{60000,{5,1,4}},{60750,{1,5,3}},{61440,{12,1,1}},{62208,{8,5,0}},{62500,{2,0,6}},{64000,{9,0,3}},{64800,{5,4,2}},{65536,{16,0,0}},{65610,{1,8,1}},{67500,{2,3,4}},{69120,{9,3,1}},{69984,{5,7,0}},{72000,{6,2,3}},{72900,{2,6,2}},{73728,{13,2,0}},{75000,{3,1,5}},{76800,{10,1,2}},{77760,{6,5,1}},{78125,{0,0,7}},{78732,{2,9,0}},{80000,{7,0,4}},{81000,{3,4,3}},{81920,{14,0,1}},{82944,{10,4,0}},{84375,{0,3,5}},{86400,{7,3,2}},{87480,{3,7,1}},{90000,{4,2,4}},{91125,{0,6,3}},{92160,{11,2,1}},{93312,{7,6,0}},{93750,{1,1,6}},{96000,{8,1,3}},{97200,{4,5,2}},{98304,{15,1,0}},{98415,{0,9,1}},{100000,{5,0,5}},{101250,{1,4,4}},{102400,{12,0,2}},{103680,{8,4,1}},{104976,{4,8,0}},{108000,{5,3,3}},{109350,{1,7,2}},{110592,{12,3,0}},{112500,{2,2,5}},{115200,{9,2,2}},{116640,{5,6,1}},{118098,{1,10,0}},{120000,{6,1,4}},{121500,{2,5,3}},{122880,{13,1,1}},{124416,{9,5,0}},{125000,{3,0,6}},{128000,{10,0,3}},{129600,{6,4,2}},{131072,{17,0,0}},{131220,{2,8,1}},{135000,{3,3,4}},{138240,{10,3,1}},{139968,{6,7,0}},{140625,{0,2,6}},{144000,{7,2,3}},{145800,{3,6,2}},{147456,{14,2,0}},{150000,{4,1,5}},{151875,{0,5,4}},{153600,{11,1,2}},{155520,{7,5,1}},{156250,{1,0,7}},{157464,{3,9,0}},{160000,{8,0,4}},{162000,{4,4,3}},{163840,{15,0,1}}};
static Factorized find_largest_no_larger_than(int number)
{
  int i=0;
  while(factorized[i].number <= number) {
    i++;
  }
  return factorized[i-1];
}

typedef struct YMPID_Topology {
  int                   cpuid;
  int                   selected;
  int                   num_board;
  Factorized            num_board_factorized;
  int                   board_size;
  Factorized            board_size_factorized;
} YMPID_Topology;

/* funcs */

#ifdef ARCH_SW

extern long sys_m_cgid();

#define BOARD_ID(PHY_ADDR) (PHY_ADDR / 256)
#define BOARD_OF(PHY_ADDR) (PHY_ADDR % 256)

static int sizeof_board[MAX_NUM_BOARD];
static int cpuid_list[MAX_NPROCS];
static int YMPID_Get_topology_sorthelper(const void* a, const void* b)
{
  int lhs = *((const int*)a);
  int rhs = *((const int*)b);
  if (lhs > rhs) {
    return 1;
  } else if (lhs == rhs) {
    return 0;
  }
  return -1;
}

static YMPID_Topology* YMPID_Get_topology(MPI_Comm comm, MPI_Comm* out_comm)
{
  YMPID_Topology* topology = (YMPID_Topology*) malloc(sizeof(YMPID_Topology));
  memset(topology, 0, sizeof(YMPID_Topology));
  memset(sizeof_board, 0, sizeof(sizeof_board));

  int i, rank, nprocs;
  MPI_Comm_rank(comm, &rank);
  MPI_Comm_size(comm, &nprocs);

  int my_cpuid;
  char hostname[256];
  gethostname(hostname, 256);
  assert(sscanf(hostname, "vn%d", &my_cpuid) == 1);
  topology->cpuid = my_cpuid;
  MPI_Allgather(&my_cpuid, 1, MPI_INT, cpuid_list, 1, MPI_INT, comm);
  qsort(cpuid_list, nprocs, sizeof(int), YMPID_Get_topology_sorthelper);
  int num_board=0;
  {
    int begin=0, last_board_id=BOARD_ID(cpuid_list[0]);
    for(i=1; i<nprocs; i++) {
      int board_id = BOARD_ID(cpuid_list[i]);
      if(board_id != last_board_id) {
        sizeof_board[num_board] = i - begin;
        begin                   = i;
        last_board_id           = board_id;
        num_board++;
      }
    }
  }
  qsort(sizeof_board, num_board, sizeof(int), YMPID_Get_topology_sorthelper);
  if(rank == 0) {
    printf("sizeof_board: ");
    for(i=0; i<num_board; i++) {
      printf("%d ", sizeof_board[i]);
    }
    printf("\n");
  }
  int target_num_board = 0, board_size = 0;
  Factorized num_board_factorized;
  {
    Factorized board_size_factorized;
    int max_np=0;
    for(i=0; i<num_board; i++) {
      if(sizeof_board[i]*(num_board-i) > max_np) {
        max_np           = sizeof_board[i] * (num_board - i);
        board_size       = sizeof_board[i];
      }
    }
    board_size_factorized = find_largest_no_larger_than(board_size);
    board_size            = board_size_factorized.number;
    int np = 0;
    for(i=0; i<num_board; i++) {
      if(sizeof_board[i] >= board_size) {
        np += board_size;
        target_num_board++;
      }
    }
    LOGDS("board_size: %d    num_procs: %d    efficiency: %.2lf%%\n", board_size, np, (double)np/nprocs);
    num_board_factorized  = find_largest_no_larger_than(target_num_board);

    topology->board_size            = board_size;
    topology->board_size_factorized = board_size_factorized;
    topology->num_board             = target_num_board;
    topology->num_board_factorized  = num_board_factorized;
  }
  int selected = 0;
  {
    int current_num_board;
    int begin=0, last_board_id=BOARD_ID(cpuid_list[0]);
    for(i=1; i<nprocs; i++) {
      int board_id = BOARD_ID(cpuid_list[i]);
      if(board_id != last_board_id) {
        int current_board_size  = i - begin;
        if(current_board_size >= board_size) {
          int j;
          for(j=begin; j<begin+board_size; j++) {
            if(cpuid_list[j] == my_cpuid) {
              selected = 1;
            }
          }
        }
        begin                   = i;
        last_board_id           = board_id;
        current_num_board++;
      }
    }
    assert(current_num_board == target_num_board);
  }
  topology->board_size *= 4;
  topology->board_size_factorized.factor.n0 += 2;
  long cgid = sys_m_cgid();
  MPI_Comm_split(comm, selected, my_cpuid*4 + cgid, out_comm);
  topology->selected = selected;
  return topology;
}
#else
#error "UNIMPLEMENTED."
#endif

static int total_iter = 0;
static int iter_group_size[MAX_NUM_ITER];
static int successive_rank[MAX_NUM_ITER][MAX_GROUP_SIZE];

int YMPI_Shuffle(uint64_t* buf, size_t buf_len, uint64_t* rst, size_t* rst_len)
{
  /* filter redundant nodes and creates new communicator, create the topology table */
  YMPID_Topology* topology;
  MPI_Comm        comm;
  topology = YMPID_Get_topology(MPI_COMM_WORLD, &comm);
  assert(topology != NULL);

  int rank, nprocs;
  MPI_Comm_rank(comm, &rank);
  MPI_Comm_size(comm, &nprocs);

  /* pre-calculates the routes */
  {
    int        num_board             = topology->num_board;
    int        board_size            = topology->board_size;
    int        my_board_id           = rank / board_size;
    int        my_board_of           = rank % board_size;
    Factorized num_board_factorized  = topology->num_board_factorized;
    Factorized board_size_factorized = topology->board_size_factorized;
    {
      int i, p;

#define GEN_ITERS_INTER_BOARD(FACTOR_ID) do{                                                                                            \
      for(i=0; i<num_board_factorized.factor.n ## FACTOR_ID; i++) {                                                                     \
        int group_size = valid_factor[FACTOR_ID];                                                                                       \
        iter_group_size[total_iter] = group_size;                                                                                       \
        for(p=0; p<group_size; p++) {                                                                                                   \
          int next_board_id = (my_board_id/(group_base*group_size)*(group_base*group_size))+(p*group_base)+(my_board_id%group_base);    \
          successive_rank[total_iter][p] = next_board_id*board_size + my_board_of;                                                      \
        }                                                                                                                               \
        group_base *= group_size;                                                                                                       \
        total_iter++;                                                                                                                   \
      }}while(0)                                                                                                                        

#define GEN_ITERS_INTRA_BOARD(FACTOR_ID) do{                                                                                            \
      for(i=0; i<board_size_factorized.factor.n ## FACTOR_ID; i++) {                                                                    \
        int group_size = valid_factor[FACTOR_ID];                                                                                       \
        iter_group_size[total_iter] = group_size;                                                                                       \
        for(p=0; p<group_size; p++) {                                                                                                   \
          int next_board_of = (my_board_of/(group_base*group_size)*(group_base*group_size))+(p*group_base)+(my_board_of%group_base);    \
          successive_rank[total_iter][p] = my_board_id*board_size + next_board_of;                                                      \
        }                                                                                                                               \
        group_base *= group_size;                                                                                                       \
        total_iter++;                                                                                                                   \
      }}while(0)                                                                                                                                                                                                                                           

      {
        int group_base = 1;
        GEN_ITERS_INTER_BOARD(0);
        GEN_ITERS_INTER_BOARD(1);
        GEN_ITERS_INTER_BOARD(2);
      }
      {
        int group_base = 1;
        GEN_ITERS_INTRA_BOARD(0);
        GEN_ITERS_INTRA_BOARD(1);
        GEN_ITERS_INTRA_BOARD(2);
      }
    }
  }
  return 0;
}

int main(void)
{
  MPI_Init(NULL, NULL);
  YMPI_Shuffle(NULL, 0, NULL, NULL);

  MPI_Finalize();
  return 0;
}
