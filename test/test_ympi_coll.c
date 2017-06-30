#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <mpi.h>

#include "common.h"
#include "ympi.h"

#define MAX_NP 256

int main(void) {
  int i, rank, nprocs;
  int iters = 1024;
  YMPI_Init(NULL, NULL);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

  MPI_Barrier(MPI_COMM_WORLD);
  MPI_Barrier(MPI_COMM_WORLD);
  MPI_Barrier(MPI_COMM_WORLD);

  {
    LOGDS("Testing fan-in correctness (%d packet)...\n", YMPI_PREPOST_DEPTH);
    if(rank != 0) {
      YMPI_Rdma_buffer send_buffer;
      uint64_t* sb = NULL;
      uintptr_t sb_ptr = 0;
      YMPI_Alloc(&send_buffer, 1024);
      YMPI_Get_buffer(send_buffer, &sb_ptr);
      sb = (uint64_t*) sb_ptr;
      assert(sb != NULL);
      for(i=0; i<YMPI_PREPOST_DEPTH; i++) {
        sb[i] = rank * YMPI_PREPOST_DEPTH + i;
        YMPI_Zsend(send_buffer, i * sizeof(uint64_t), sizeof(uint64_t), 0);
      }
      YMPI_Zflush();
      YMPI_Dealloc(&send_buffer);
    } else {
      int count[MAX_NP];
      memset(count, 0, MAX_NP * sizeof(int));
      uint64_t* recv_buffers[YMPI_PREPOST_DEPTH];
      uint64_t  recv_buffers_len[YMPI_PREPOST_DEPTH];
      memset(recv_buffers, 0, sizeof(recv_buffers));
      memset(recv_buffers_len, 0, sizeof(recv_buffers_len));
      int k;
      for(k=0; k<nprocs; k++) {
        if(k != rank) {
          for(i=0; i<YMPI_PREPOST_DEPTH; i++) {
            YMPI_Zrecv(&recv_buffers[i], &recv_buffers_len[i], k);
            assert(recv_buffers_len[i] == sizeof(uint64_t));
            uint64_t elem = *(recv_buffers[i]);
            int src = elem / YMPI_PREPOST_DEPTH;
            int id  = elem % YMPI_PREPOST_DEPTH;
            assert(count[src] == id);
            count[src]++;
          }
          YMPI_Return();
        }
      }
      for(k=0; k<nprocs; k++) {
        if(k != rank) {
          assert(count[k] == YMPI_PREPOST_DEPTH);
        } else {
          assert(count[k] == 0);
        }
      }
    }
    MPI_Barrier(MPI_COMM_WORLD);
    LOGDS("  passed\n");
  }

  {
    LOGDS("Testing all-to-all correctness (%d packet)...\n", YMPI_PREPOST_DEPTH);
    // send
    YMPI_Rdma_buffer send_buffer;
    {
      uint64_t* sb = NULL;
      uintptr_t sb_ptr = 0;
      YMPI_Alloc(&send_buffer, nprocs * YMPI_PREPOST_DEPTH * sizeof(uint64_t));
      YMPI_Get_buffer(send_buffer, &sb_ptr);
      sb = (uint64_t*) sb_ptr;
      assert(sb != NULL);
      int dest;
      for(dest=0; dest<nprocs; dest++) {
        if(dest != rank) {
          for(i=0; i<YMPI_PREPOST_DEPTH; i++) {
            sb[dest * YMPI_PREPOST_DEPTH + i] = rank * YMPI_PREPOST_DEPTH + i;
            YMPI_Post_send(send_buffer, (dest * YMPI_PREPOST_DEPTH + i) * sizeof(uint64_t), sizeof(uint64_t), dest);
          }
        }
      }
    } 
    {
      int count[MAX_NP];
      memset(count, 0, MAX_NP * sizeof(int));
      uint64_t* recv_buffers[YMPI_PREPOST_DEPTH];
      uint64_t  recv_buffers_len[YMPI_PREPOST_DEPTH];
      memset(recv_buffers, 0, sizeof(recv_buffers));
      memset(recv_buffers_len, 0, sizeof(recv_buffers_len));
      int k;
      for(k=0; k<(nprocs-1); k++) {
        // semantic does not support this scenario
        YMPI_Expect(0, YMPI_PREPOST_DEPTH, recv_buffers, recv_buffers_len);
        for(i=0; i<YMPI_PREPOST_DEPTH; i++) {
          assert(recv_buffers_len[i] == sizeof(uint64_t));
          uint64_t elem = *(recv_buffers[i]);
          int src = elem / YMPI_PREPOST_DEPTH;
          int id  = elem % YMPI_PREPOST_DEPTH;
          assert(count[src] == id);
          count[src]++;
        }
        YMPI_Return();
      }
      for(k=0; k<nprocs; k++) {
        if(k != rank) {
          assert(count[k] == YMPI_PREPOST_DEPTH);
        } else {
          assert(count[k] == 0);
        }
      }
    }
    MPI_Barrier(MPI_COMM_WORLD);
    YMPI_Dealloc(&send_buffer);
    MPI_Barrier(MPI_COMM_WORLD);
    LOGDS("  passed\n");
  }

  YMPI_Finalize();
  return 0;
}
