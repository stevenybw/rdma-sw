#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include "ympi.h"

#define MAX_ITER 16*1024

#define LOGD(...) do {int rank;MPI_Comm_rank(MPI_COMM_WORLD, &rank); printf("[%4d]", rank); printf(__VA_ARGS__); }while(0)
#define LOGDS(...) do {int rank;MPI_Comm_rank(MPI_COMM_WORLD, &rank);if (rank==0) {printf("[%4d]", rank); printf(__VA_ARGS__);}}while(0)

static inline void do_pingpong(YMPI_Rdma_buffer send_buffer, int batch_size, int rank, int np, int np_mask, int nb)
{
  // sender
  if(rank == 0) {
    YMPI_Zsend(send_buffer, 0, nb, 1);
    YMPI_Zflush();
    void* ptr;
    uint64_t len;
    YMPI_Zrecv(&ptr, &len, 1);
  } else if (rank == 1) {
    void* ptr;
    uint64_t len;
    YMPI_Zrecv(&ptr, &len, 0);
    YMPI_Zsend(send_buffer, 0, nb, 0);
    YMPI_Zflush();
  }
  YMPI_Return();
}

static inline void do_pingpong_header(int rank)
{
}

typedef struct iter_time_pair {
  int iter;
  double time;
} iter_time_pair;

static int compare_double(const void* a, const void* b)
{
  double lhs = *((const double*) a);
  double rhs = *((const double*) b);
  if(lhs > rhs) {
    return 1;
  } else if (lhs == rhs) {
    return 0;
  } else {
    return -1;
  }
}

static int compare_iter_time_pair(const void* a, const void* b)
{
  iter_time_pair lhs = *((const iter_time_pair*) a);
  iter_time_pair rhs = *((const iter_time_pair*) b);
  return compare_double(&(lhs.time), &(rhs.time));
}

double timing_result[MAX_ITER];
iter_time_pair ordered_timing_result[MAX_ITER];

static inline void do_pingpong_output(int batch_size, int rank, int np, int np_mask, int nb, int iter, int skip, double time)
{
  if(rank == 0) {
    int i;
    for(i=0; i<iter; i++) {
      timing_result[i] = 1e6 * 0.5 * timing_result[i];
      ordered_timing_result[i] = (iter_time_pair) {i, timing_result[i]};
    }
    qsort(ordered_timing_result, iter, sizeof(iter_time_pair), compare_iter_time_pair);
    printf("nprocs=%d  bytes=%d  median(us)=%lf\n", np, nb, ordered_timing_result[iter/2].time);
    for(i=0; i<iter; i++) {
      printf("  %16d%16lf(%16lf,%8d)\n", i, timing_result[i], ordered_timing_result[i].time, ordered_timing_result[i].iter);
    }
  }
}

#define TEST(ACTION, NPROCS) do {                                                                   \
ACTION ## _header(rank);                                                                            \
for(np=2; np<=NPROCS; np*=2) {                                                                      \
  MPI_Comm comm;                                                                                    \
  MPI_Comm_split(MPI_COMM_WORLD, rank<np, rank, &comm);                                             \
  if(rank < np) {                                                                                   \
    int np_mask = np-1;                                                                             \
    for(nb=1; nb<=bytes; nb*=2) {                                                                  \
      int i;                                                                                        \
      for(i=0; i<iter; i++) {                                                                       \
        MPI_Barrier(comm);                                                                            \
        double duration = -MPI_Wtime();                                                             \
        ACTION(send_buffer, batch_size, rank, np, np_mask, nb);                                     \
        duration += MPI_Wtime();                                                                      \
        timing_result[i] = duration;                                                                \
      }                                                                                             \
      MPI_Barrier(comm);                                                                            \
      ACTION ## _output(batch_size, rank, np, np_mask, nb, iter, skip, 0.0);                        \
    }                                                                                               \
  }                                                                                                 \
  MPI_Comm_free(&comm);                                                                             \
}                                                                                                   \
}while(0)

int main(void)
{
  MPI_Init(NULL, NULL);
  YMPI_Init(NULL, NULL);

  int np, nb;
  int rank, nprocs, bytes=4*1024, iter=MAX_ITER, batch_size=0, skip=32;

  LOGDS("MAX_ITER: %d\n", MAX_ITER);

  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

  void* sb;
  uintptr_t sb_ptr = 0;
  YMPI_Rdma_buffer send_buffer;
  YMPI_Alloc(&send_buffer, nprocs * bytes);
  YMPI_Get_buffer(send_buffer, &sb_ptr);
  sb = (void*) sb_ptr;
  memset(sb, 0, nprocs * bytes);

  MPI_Barrier(MPI_COMM_WORLD);
  if(rank == 0) {
    printf("TEST: Pingpong latency\n");
  }
  TEST(do_pingpong, 2);

  YMPI_Finalize();
  MPI_Finalize();
  return 0;
}