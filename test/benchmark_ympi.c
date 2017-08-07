#include <stdio.h>
#include <string.h>
#include <mpi.h>
#include "ympi.h"

static inline void do_allputall(YMPI_Rdma_buffer send_buffer, int batch_size, int rank, int np, int np_mask, int nb)
{
  int k, q;
  if(rank < np) {
    for(q=(rank+1)&np_mask; q!=rank; q=(q+1)&np_mask) {
      YMPI_Zsend(send_buffer, q*nb, nb, q);
    }
    YMPI_Zflush();
    for(q=(rank+1)&np_mask; q!=rank; q=(q+1)&np_mask) {
      void* ptr;
      uint64_t len;
      YMPI_Zrecv(&ptr, &len, q);
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

static inline void do_allputall_output(int batch_size, int rank, int np, int np_mask, int nb, int iter, int skip, double time)
{
  if(rank == 0) {
    int nsend_mesg  = (np-1) * (iter-skip);
    double msg_rate = (double)nsend_mesg / time / 1000.0 / 1000.0;
    printf("%16d%16d%16.2f\n", np, nb, msg_rate);
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

static inline void do_fanout(YMPI_Rdma_buffer send_buffer, int batch_size, int rank, int np, int np_mask, int nb)
{
  // sender
  if(rank == 0) {
    int i;
    for(i=1; i<np; i++) {
      YMPI_Zsend(send_buffer, 0, nb, i);
    }
    YMPI_Zflush();
  } else if (rank < np) {
    void* ptr;
    uint64_t len;
    YMPI_Zrecv(&ptr, &len, 0);
    YMPI_Return();
  }
}

static inline void do_fanout_header(int rank)
{
  if(rank == 0) {
    printf("%16s%16s%16s\n", "nprocs", "bytes", "Mmesg/s");
  }
}

static inline void do_fanout_output(int batch_size, int rank, int np, int np_mask, int nb, int iter, int skip, double time)
{
  if(rank == 0) {
    int nsend_mesg  = (np-1) * (iter-skip);
    double msg_rate = (double)nsend_mesg / time / 1000.0 / 1000.0;
    printf("%16d%16d%16.2f\n", np, nb, msg_rate);
  }
}

static inline void do_fanin(YMPI_Rdma_buffer send_buffer, int batch_size, int rank, int np, int np_mask, int nb)
{
  // receiver
  if(rank == 0) {
    int i;
    for(i=1; i<np; i++) {
      void* ptr;
      uint64_t len;
      YMPI_Zrecv(&ptr, &len, i);
    }
    YMPI_Return();
  } else if (rank < np) {
    YMPI_Zsend(send_buffer, 0, nb, 0);
    YMPI_Zflush();
  }
}

static inline void do_fanin_header(int rank)
{
  if(rank == 0) {
    printf("%16s%16s%16s\n", "nprocs", "bytes", "Mmesg/s");
  }
}

static inline void do_fanin_output(int batch_size, int rank, int np, int np_mask, int nb, int iter, int skip, double time)
{
  if(rank == 0) {
    int nsend_mesg  = (np-1) * (iter-skip);
    double msg_rate = (double)nsend_mesg / time / 1000.0 / 1000.0;
    printf("%16d%16d%16.2f\n", np, nb, msg_rate);
  }
}

#define TEST(ACTION, NPROCS) do {                                                                   \
ACTION ## _header(rank);                                                                            \
for(np=2; np<=NPROCS; np*=2) {                                                                      \
  MPI_Comm comm;                                                                                    \
  MPI_Comm_split(MPI_COMM_WORLD, rank<np, rank, &comm);                                             \
  if(rank < np) {                                                                                   \
    int np_mask = np-1;                                                                             \
    for(nb=32; nb<=bytes; nb*=2) {                                                                  \
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
  int rank, nprocs, bytes=4*1024, iter=16, batch_size=8, skip=0;

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
    printf("TEST 1: Fan-out throughput\n");
  }
  TEST(do_fanout, nprocs);

  MPI_Barrier(MPI_COMM_WORLD);
  if(rank == 0) {
    printf("TEST 2: Fan-in  throughput\n");
  }
  TEST(do_fanin, nprocs);

  MPI_Barrier(MPI_COMM_WORLD);
  if(rank == 0) {
    printf("TEST 3: Pingpong latency\n");
  }
  TEST(do_pingpong, 2);

  MPI_Barrier(MPI_COMM_WORLD);
  if(rank == 0) {
    printf("TEST 4: All-put-all throughput\n");
  }
  TEST(do_allputall, nprocs);


  YMPI_Finalize();
  MPI_Finalize();
  return 0;
}