#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

#include <mpi.h>
#include "ympi.h"
#include "common.h"

#define MAX_NPROCS 1024
#define NEXT_SEED(SEED) ((1103515245 * SEED + 12345) % 2^32);

typedef struct test1_server_args
{
  int   id;
  int   num_iter;
  int   size;
  char *sb;
  int   nprocs;
  int  *rkeys;
  double *wsec;
  uintptr_t *base_ptrs;
  YMPI_Rdma_buffer send_buffer;
} test1_server_args;

void* test1_server(void* input_args)
{
  test1_server_args* args = (test1_server_args*) input_args;
  int  id       = args->id;
  int  num_iter = args->num_iter;
  int  size     = args->size;
  int  nprocs   = args->nprocs;
  int *rkeys    = args->rkeys;
  uintptr_t *base_ptrs = args->base_ptrs;
  YMPI_Rdma_buffer send_buffer = args->send_buffer;
  int i;
  uint32_t seed = id;
  for(i=0; i<1; i++) {
    // warm-up
    int dest = 1 + (seed % (nprocs-1));
    seed = NEXT_SEED(seed);
    YMPI_Write(send_buffer, dest*size, size, dest, rkeys[dest], (void*) base_ptrs[dest]);
    YMPI_Zflush();
  }
  double wsec = -MPI_Wtime();
  for(i=0; i<num_iter; i++) {
    int dest = 1 + (seed % (nprocs-1));
    seed = NEXT_SEED(seed);
    YMPI_Write(send_buffer, dest*size, size, dest, rkeys[dest], (void*) base_ptrs[dest]);
    YMPI_Zflush();
  }
  wsec += MPI_Wtime();
  (*args->wsec) = wsec;
  return NULL;
}

typedef struct test1_client_args
{
  int server_rkey;
  uintptr_t server_base_ptr;
} test1_client_args;

int main(int argc, char* argv[])
{
  MPI_Init(&argc, &argv);
  YMPI_Init(&argc, &argv);

  int rank, nprocs;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

  MPI_Barrier(MPI_COMM_WORLD);
  {
    int num_threads = 4;
    int num_iter    = 2*1024*1024;
    int size        = 32;
    LOGDS("Test 1: Fan-out based on RDMA_Write (num_threads=%d, num_iter=%d, size=%d)\n", num_threads, num_iter, size);
    // receiver
    if(rank == 0) {
      pthread_t server_threads[num_threads];
      test1_server_args args_list[num_threads];
      double    wsec_list[num_threads];
      memset(wsec_list, 0, sizeof(wsec_list));

      YMPI_Rdma_buffer send_buffer;
      char* sb;
      void* base_ptrs[MAX_NPROCS];
      int   my_rkey;
      int   rkeys[MAX_NPROCS];
      uint64_t sb_ptr;
      YMPI_Allocate(&send_buffer, size * nprocs, YMPI_BUFFER_TYPE_REMOTE_ATOMIC);
      YMPI_Get_buffer(send_buffer, &sb_ptr);
      YMPI_Get_rkey(send_buffer, &my_rkey);
      sb = (char*) sb_ptr;
      int i, j;
      for(i=0; i<nprocs; i++) {
        for(j=0; j<size; j++) {
          sb[i*size + j] = i;
        }
      }
      MPI_Allgather(&sb_ptr, 1, MPI_UNSIGNED_LONG_LONG, base_ptrs, 1, MPI_UNSIGNED_LONG_LONG, MPI_COMM_WORLD);
      MPI_Allgather(&my_rkey, 1, MPI_INT, rkeys, 1, MPI_INT, MPI_COMM_WORLD);

      for(i=0; i<num_threads; i++) {
        args_list[i] = (test1_server_args){
          .id = i,
          .num_iter = num_iter,
          .size = size,
          .sb = sb,
          .nprocs = nprocs,
          .rkeys  = rkeys,
          .base_ptrs = (uintptr_t*) base_ptrs,
          .send_buffer = send_buffer,
          .wsec = &wsec_list[i],
        };
        pthread_create(&server_threads[i], NULL, test1_server, &args_list[i]);
      }

      double sum_iops = 0.0;
      for(i=0; i<num_threads; i++) {
        pthread_join(server_threads[i], NULL);
        double iops = (double)num_iter / wsec_list[i];
        sum_iops += iops;
        LOGD("  thread %d: iops=%lf Mmesg/s\n", i, 1e-6*iops);
      }
      LOGD("  sum: iops=%lf Mmesg/s\n", 1e-6*sum_iops);
      MPI_Barrier(MPI_COMM_WORLD);
      YMPI_Dealloc(&send_buffer);
    } else {
      YMPI_Rdma_buffer recv_buffer;
      char* rb;
      void* base_ptrs[MAX_NPROCS];
      void* rkeys[MAX_NPROCS];
      uintptr_t rb_ptr;
      int my_rkey;
      YMPI_Allocate(&recv_buffer, size, YMPI_BUFFER_TYPE_REMOTE_ATOMIC);
      YMPI_Get_buffer(recv_buffer, &rb_ptr);
      YMPI_Get_rkey(recv_buffer, &my_rkey);
      rb = (char*) rb_ptr;
      MPI_Allgather(&rb_ptr, 1, MPI_UNSIGNED_LONG_LONG, base_ptrs, 1, MPI_UNSIGNED_LONG_LONG, MPI_COMM_WORLD);
      MPI_Allgather(&my_rkey, 1, MPI_INT, rkeys, 1, MPI_INT, MPI_COMM_WORLD);
      MPI_Barrier(MPI_COMM_WORLD);
      int i;
      for(i=0; i<size; i++) {
        int val = rb[i];
        if(val != rank) {
          LOGD("!!! ERROR rb[%d]   expected=%d  actual=%d\n", i, rank, val);
        }
      }
      YMPI_Dealloc(&recv_buffer);
    }
  }

  YMPI_Finalize();
  MPI_Finalize();

  return 0;
}