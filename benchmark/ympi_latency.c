#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include "ympi.h"

#define MAX_ITER 1024

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
  if(rank == 0) {
    printf("%16s%16s%16s\n", "nprocs", "bytes", "median(us)");
  }
}

double timing_result[MAX_ITER];

static inline void do_pingpong_output(int batch_size, int rank, int np, int np_mask, int nb, int iter, int skip, double time)
{
  if(rank == 0) {
    qsort(timing_result, iter, sizeof(double), compare_double);
    int i;
    for(i=0; i<iter; i++) {
      printf("  %16d%16lf\n", i, 1e6 * timing_result[i]);
    }
    printf("%16d%16d%16.2f\n", np, nb, timing_result[iter/2]);
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
  int rank, nprocs, bytes=4*1024, iter=1024, batch_size=0, skip=32;

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