#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <mpi.h>
#include "ympi.h"
#include "common.h"

#define MAX_NUM_PROCS (4*40*1024)
#define ALLPUTALL_NUMPROCS (128)

#define DISTANCE (1)

static int local_rank_to_global_rank[MAX_NUM_PROCS];
static int target_rank_list[MAX_NUM_PROCS];
static int target_rank_list_1[MAX_NUM_PROCS];

static int shuffle_rank(int rank, int nprocs)
{
  unsigned long long result = rank;
  result = (result * DISTANCE) % nprocs;
  return (int)result;
}

static inline void do_allputall(YMPI_Rdma_buffer send_buffer, int batch_size, int rank, int np, int np_mask, int nb)
{
  int i, q;
  if(rank < np) {
    int group_base = rank/ALLPUTALL_NUMPROCS*ALLPUTALL_NUMPROCS;
    int group_off  = rank%ALLPUTALL_NUMPROCS;
    for(i=1; i<ALLPUTALL_NUMPROCS; i++) {
      int q = group_base + (group_off + i) % ALLPUTALL_NUMPROCS;
      assert(q < np);
      YMPI_Zsend(send_buffer, i*nb, nb, local_rank_to_global_rank[q]);
      // YMPI_Zsend(send_buffer, i*nb, nb, q);
    }
    YMPI_Zflush();
    for(i=1; i<ALLPUTALL_NUMPROCS; i++) {
      void* ptr;
      uint64_t len;
      int q = group_base + (group_off + i) % ALLPUTALL_NUMPROCS;
      assert(q < np);
      YMPI_Zrecv(&ptr, &len, local_rank_to_global_rank[q]);
      // YMPI_Zrecv(&ptr, &len, q);
    }
    YMPI_Return();
  }
}

static inline void do_allputall_header(int rank)
{
  if(rank == 0) {
    printf("%16s%16s%16s\n", "nprocs", "bytes", "Mmesg/s(per proc)");
  }
}

static inline void do_allputall_output(MPI_Comm comm, int batch_size, int rank, int np, int np_mask, int nb, int iter, int skip, double time)
{
  int nprocs;
  MPI_Comm_size(comm, &nprocs);
  assert(nprocs == np);
  double total_time;
  MPI_Allreduce(&time, &total_time, 1, MPI_DOUBLE, MPI_SUM, comm);
  double avg_time = total_time/np;
  if(rank == 0) {
    int nsend_mesg  = (ALLPUTALL_NUMPROCS-1) * (iter-skip);
    double msg_rate = (double)nsend_mesg / avg_time / 1000.0 / 1000.0;
    printf("%16d%16d%16.2f\n", np, nb, msg_rate);
  }
}

#define TEST_SHUFFLE(ACTION, NPROCS) do {                                                           \
int rank   = shuffled_rank;                                                                         \
int nprocs = shuffled_nprocs;                                                                       \
ACTION ## _header(rank);                                                                            \
for(np=ALLPUTALL_NUMPROCS; np<=NPROCS; np*=2) {                                                     \
  MPI_Comm comm;                                                                                    \
  MPI_Comm_split(shuffled_comm_world, rank<np, rank, &comm);                                        \
  if(rank < np) {                                                                                   \
    int np_mask = np-1;                                                                             \
    for(nb=1; nb<=bytes; nb*=2) {                                                                   \
      MPI_Barrier(comm);                                                                            \
      int i;                                                                                        \
      double duration = 0;                                                                          \
      for(i=0; i<iter; i++) {                                                                       \
        if(i==skip) {                                                                               \
          MPI_Barrier(comm);                                                                        \
          duration -= MPI_Wtime();                                                                  \
        }                                                                                           \
        ACTION(send_buffer, batch_size, rank, np, np_mask, nb);                                     \
      }                                                                                             \
      duration += MPI_Wtime();                                                                      \
                                                                                                    \
      MPI_Barrier(comm);                                                                            \
      ACTION ## _output(comm, batch_size, rank, np, np_mask, nb, iter, skip, duration);             \
    }                                                                                               \
  }                                                                                                 \
  MPI_Comm_free(&comm);                                                                             \
}                                                                                                   \
}while(0)

/*
int Check_precondition()
{
  int nprocs;
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  while(nprocs % 2 == 0) {
    nprocs /= 2;
  }
  assert(nprocs==1);
}
*/

int main(void)
{
  MPI_Init(NULL, NULL);

  // Check_precondition();

  int np, nb;
  int rank, nprocs, bytes=4*1024, iter=2, batch_size=8, skip=0;
  MPI_Comm shuffled_comm_world;
  int shuffled_rank, shuffled_nprocs;

  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

  MPI_Barrier(MPI_COMM_WORLD);
  if(rank == 0) {
    printf("TEST 4: All-put-all throughput\n");
    printf("  SHUFFLE_DISTANCE: %d\n", DISTANCE);
  }
  MPI_Barrier(MPI_COMM_WORLD);
  
  MPI_Comm_split(MPI_COMM_WORLD, 0, shuffle_rank(rank, nprocs), &shuffled_comm_world);
  MPI_Comm_rank(shuffled_comm_world, &shuffled_rank);
  MPI_Comm_size(shuffled_comm_world, &shuffled_nprocs);
  assert(shuffled_nprocs == nprocs);

  MPI_Barrier(MPI_COMM_WORLD);
  if(rank == 0) {
    printf("  MPI_Comm_split\n");
  }

  {
    int i;
    memset(target_rank_list, 0, sizeof(target_rank_list));

    // allputall_shuffled
    int group_base = shuffled_rank/ALLPUTALL_NUMPROCS*ALLPUTALL_NUMPROCS;
    int group_off  = shuffled_rank%ALLPUTALL_NUMPROCS;
    MPI_Allgather(&rank, 1, MPI_INT, local_rank_to_global_rank, 1, MPI_INT, shuffled_comm_world);
    for(i=1; i<ALLPUTALL_NUMPROCS; i++) {
      int dest = group_base + (group_off + i) % ALLPUTALL_NUMPROCS; 
      target_rank_list[local_rank_to_global_rank[dest]] = 1;
    }
    // LOGD("group_base=%d  group_off=%d\n", group_base, group_off);
  }
  {
    LOGDS("local_rank_to_global_rank: \n");
    int i=0;
    for(i=0; i<nprocs; i++) {
      LOGDS("%d ", local_rank_to_global_rank[i]);
    }
    LOGDS("\n");
  }

  YMPI_Init_ranklist(NULL, NULL, target_rank_list);
  //YMPI_Init(NULL, NULL);

  void* sb;
  uintptr_t sb_ptr = 0;
  YMPI_Rdma_buffer send_buffer;
  YMPI_Alloc(&send_buffer, ALLPUTALL_NUMPROCS * bytes);
  YMPI_Get_buffer(send_buffer, &sb_ptr);
  sb = (void*) sb_ptr;
  memset(sb, 0, ALLPUTALL_NUMPROCS * bytes);

  MPI_Barrier(MPI_COMM_WORLD);
  TEST_SHUFFLE(do_allputall, nprocs);

  YMPI_Finalize();
  MPI_Finalize();
  return 0;
}