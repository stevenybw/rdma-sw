#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <mpi.h>

#include "common.h"
#include "ympi.h"

int main(void) {
  int i, rank, nprocs;
  int iters = 1024;
  YMPI_Init(NULL, NULL);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  assert(nprocs == 2);

  MPI_Barrier(MPI_COMM_WORLD);
  MPI_Barrier(MPI_COMM_WORLD);
  MPI_Barrier(MPI_COMM_WORLD);

  {
    LOGDS("Testing point-to-point correctness (%d packet)...\n", YMPI_PREPOST_DEPTH);
    if(rank == 0) {
      YMPI_Rdma_buffer send_buffer;
      uint64_t* sb = NULL;
      uintptr_t sb_ptr = 0;
      YMPI_Alloc(&send_buffer, 1024);
      YMPI_Get_buffer(send_buffer, &sb_ptr);
      sb = (uint64_t*) sb_ptr;
      assert(sb != NULL);
      for(i=0; i<YMPI_PREPOST_DEPTH; i++) {
        sb[i] = 0x1111111111111111 + i;
        YMPI_Zsend(send_buffer, i * sizeof(uint64_t), sizeof(uint64_t), 1);
      }
      //YMPI_Expect(YMPI_PREPOST_DEPTH, 0, NULL, NULL);
      YMPI_Zflush();
      YMPI_Dealloc(&send_buffer);
    } else if(rank == 1) {
      uint64_t* recv_buffers[YMPI_PREPOST_DEPTH];
      uint64_t  recv_buffers_len[YMPI_PREPOST_DEPTH];
      memset(recv_buffers, 0, sizeof(recv_buffers));
      memset(recv_buffers_len, 0, sizeof(recv_buffers_len));
      //YMPI_Expect(0, YMPI_PREPOST_DEPTH, recv_buffers, recv_buffers_len);

      for(i=0; i<YMPI_PREPOST_DEPTH; i++) {
        YMPI_Zrecv(&recv_buffers[i], &recv_buffers_len[i], 0);
        printf("recv_buffers_len[%d]  = %llu\n", i, recv_buffers_len[i]);
        assert(recv_buffers_len[i] == sizeof(uint64_t));
        printf("recv_buffers[%d]    = %p (*%llu)\n", i, recv_buffers[i], *(recv_buffers[i]));
        assert(*(recv_buffers[i]) == (0x1111111111111111 + i));
      }
      YMPI_Return();
    }
  }

  MPI_Barrier(MPI_COMM_WORLD);

  {
    LOGDS("Benchmarking message rate (%d iters, %d mesg/iter, tx_depth=%d)...\n", iters, YMPI_PREPOST_DEPTH, YMPI_PREPOST_DEPTH);
    double wsec;
    int k;
    if(rank == 0) {
      YMPI_Rdma_buffer send_buffer;
      uint64_t* sb = NULL;
      uintptr_t sb_ptr = 0;
      YMPI_Alloc(&send_buffer, 1024);
      YMPI_Get_buffer(send_buffer, &sb_ptr);
      sb = (uint64_t*) sb_ptr;
      assert(sb != NULL);

      wsec = -MPI_Wtime();
      for(k=0; k<iters; k++) {
        for(i=0; i<YMPI_PREPOST_DEPTH; i++) {
          sb[i] = 0x1111111111111111 + i;
          YMPI_Zsend(send_buffer, i * sizeof(uint64_t), sizeof(uint64_t), 1);
        }
        YMPI_Zflush();
      }
      wsec += MPI_Wtime();

      YMPI_Dealloc(&send_buffer);
    } else if(rank == 1) {
      uint64_t* recv_buffers[YMPI_PREPOST_DEPTH];
      uint64_t  recv_buffers_len[YMPI_PREPOST_DEPTH];
      memset(recv_buffers, 0, sizeof(recv_buffers));
      memset(recv_buffers_len, 0, sizeof(recv_buffers_len));

      wsec = -MPI_Wtime();
      for(k=0; k<iters; k++) {
        for(i=0; i<YMPI_PREPOST_DEPTH; i++) {
          YMPI_Zrecv(&recv_buffers[i], &recv_buffers_len[i], 0);
        }
        YMPI_Return();
      }
      wsec += MPI_Wtime();
    }
    MPI_Barrier(MPI_COMM_WORLD);
    LOGD("time=%lf us,  msg_rate=%lf Mmesg/s\n", wsec, 1e-6 * iters * YMPI_PREPOST_DEPTH / wsec);
  }

  MPI_Barrier(MPI_COMM_WORLD);

  {
    LOGDS("Testing point-to-point zero-bytes correctness (%d packet)...\n", YMPI_PREPOST_DEPTH);
    if(rank == 0) {
      YMPI_Rdma_buffer send_buffer;
      uint64_t* sb = NULL;
      uintptr_t sb_ptr = 0;
      YMPI_Alloc(&send_buffer, 1024);
      YMPI_Get_buffer(send_buffer, &sb_ptr);
      sb = (uint64_t*) sb_ptr;
      assert(sb != NULL);
      for(i=0; i<YMPI_PREPOST_DEPTH; i++) {
        sb[i] = 0x1111111111111111 + i;
        YMPI_Zsend(send_buffer, i * sizeof(uint64_t), 0, 1);
      }
      YMPI_Zflush();
      YMPI_Dealloc(&send_buffer);
    } else if(rank == 1) {
      uint64_t* recv_buffers[YMPI_PREPOST_DEPTH];
      uint64_t  recv_buffers_len[YMPI_PREPOST_DEPTH];
      memset(recv_buffers, 0, sizeof(recv_buffers));
      memset(recv_buffers_len, 0, sizeof(recv_buffers_len));

      for(i=0; i<YMPI_PREPOST_DEPTH; i++) {
        YMPI_Zrecv(&recv_buffers[i], &recv_buffers_len[i], 0);
        printf("recv_buffers_len[%d]  = %llu\n", i, recv_buffers_len[i]);
        assert(recv_buffers_len[i] == 0);
        printf("recv_buffers[%d]    = %p [%llu]\n", i, recv_buffers[i], *(recv_buffers[i]));
      }
      YMPI_Return();
    }
  }

  YMPI_Finalize();
  return 0;
}
