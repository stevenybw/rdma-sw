#include <stdio.h>
#include <string.h>
#include <mpi.h>
#include "ympi.h"

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
    printf("%16s%16s%16s\n", "nprocs", "bytes", "us");
  }
}

static inline void do_pingpong_output(int batch_size, int rank, int np, int np_mask, int nb, int iter, int skip, double time)
{
  if(rank == 0) {
    double latency = (double) 1e6 * time / (iter - skip) / 2.0;
    printf("%16d%16d%16.2f\n", np, nb, latency);
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
      ACTION ## _output(batch_size, rank, np, np_mask, nb, iter, skip, duration);                   \
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