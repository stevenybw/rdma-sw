#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <mpi.h>

#include "common.h"
#include "ympi.h"

#define SHOW_DETAIL 0
#define RDMA_NUM_ELEM (YMPI_MAX_SEND_WR_PER_QP-1)

#define MPI_TAG     1234

int main(void) {
  int i, rank, nprocs;
  int iters = 1024;
  MPI_Init(NULL, NULL);
  YMPI_Init(NULL, NULL);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  assert(nprocs == 2);

  MPI_Barrier(MPI_COMM_WORLD);
  MPI_Barrier(MPI_COMM_WORLD);
  MPI_Barrier(MPI_COMM_WORLD);

  LOGDS("*** YMPI Correctness Testing... (CORRECT if no assertion) ***\n");

  {
    int k;
    LOGDS("Testing point-to-point correctness (%d packet)...\n", YMPI_MAX_SEND_WR_PER_QP);
    if(rank == 0) {
      YMPI_Rdma_buffer send_buffer;
      uint64_t* sb = NULL;
      uintptr_t sb_ptr = 0;
      ZERO(YMPI_Alloc(&send_buffer, YMPI_MAX_SEND_WR_PER_QP * sizeof(int64_t)));
      ZERO(YMPI_Get_buffer(send_buffer, &sb_ptr));
      sb = (uint64_t*) sb_ptr;
      assert(sb != NULL);
      for(k=0; k<iters; k++) {
        for(i=0; i<YMPI_MAX_SEND_WR_PER_QP; i++) {
          sb[i] = 0x1111111111111111 + i;
          ZERO(YMPI_Zsend(send_buffer, i * sizeof(uint64_t), sizeof(uint64_t), 1));
        }
        ZERO(YMPI_Zflush());
      }
      ZERO(YMPI_Dealloc(&send_buffer));
    } else if(rank == 1) {
      uint64_t* recv_buffers[YMPI_MAX_SEND_WR_PER_QP];
      uint64_t  recv_buffers_len[YMPI_MAX_SEND_WR_PER_QP];
      memset(recv_buffers, 0, sizeof(recv_buffers));
      memset(recv_buffers_len, 0, sizeof(recv_buffers_len));
      for(k=0; k<iters; k++) {
        for(i=0; i<YMPI_MAX_SEND_WR_PER_QP; i++) {
          YMPI_Zrecv(&recv_buffers[i], &recv_buffers_len[i], 0);

          #if SHOW_DETAIL
            printf("  recv_buffers_len[%d]  = %llu\n", i, recv_buffers_len[i]);
            printf("  recv_buffers[%d]    = %p (*%llu)\n", i, recv_buffers[i], *(recv_buffers[i]));
          #endif

          assert(recv_buffers_len[i] == sizeof(uint64_t));
          assert(*(recv_buffers[i]) == (0x1111111111111111 + i));
        }
        YMPI_Return();
      }
    }
  }

  YMPI_Zflush();
  MPI_Barrier(MPI_COMM_WORLD);

  {
    int total_io = 8*1024*1024;
    int k;
    LOGDS("Testing RDMA Write correctness (%d packet)...\n", total_io);
    if(rank == 0) {
      YMPI_Rdma_buffer send_buffer;
      uint64_t* sb   = NULL;
      uint32_t  rkey = 0;
      uint64_t* rb   = NULL;
      uintptr_t sb_ptr = 0;
      YMPI_Allocate(&send_buffer, total_io * sizeof(int64_t), YMPI_BUFFER_TYPE_REMOTE_ATOMIC);
      YMPI_Get_buffer(send_buffer, &sb_ptr);
      sb = (uint64_t*) sb_ptr;
      MPI_Recv(&rb, 1, MPI_UNSIGNED_LONG_LONG, 1, MPI_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      MPI_Recv(&rkey, 1, MPI_UNSIGNED, 1, MPI_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      assert(sb != NULL);
      assert(rb != NULL);

      uint64_t* sig = &rb[0];
      rb = &rb[YMPI_PAGE_SIZE / sizeof(int64_t)];
      for(i=0; i<total_io; i++) {
        sb[i] = 0x1111111111111111 + i;
      }
      double wsec = -MPI_Wtime();
      for(i=0; i<total_io; i++) {
        YMPI_Write(send_buffer, i * sizeof(uint64_t), sizeof(uint64_t), 1, rkey, &rb[i]);
      }
      YMPI_Zflush();
      wsec += MPI_Wtime();
      LOGD("  time=%lf us,  msg_rate=%lf Mmesg/s\n", wsec, 1e-6 * total_io / wsec);
      YMPI_Write(send_buffer, 0, sizeof(uint64_t), 1, rkey, sig);
      YMPI_Dealloc(&send_buffer);
    } else if(rank == 1) {
      YMPI_Rdma_buffer recv_buffer;
      uint32_t  rkey = 0;
      uint64_t* rb = NULL;
      uintptr_t rb_ptr = 0;
      YMPI_Allocate(&recv_buffer, YMPI_PAGE_SIZE + total_io * sizeof(int64_t), YMPI_BUFFER_TYPE_REMOTE_ATOMIC);
      YMPI_Get_buffer(recv_buffer, &rb_ptr);
      YMPI_Get_rkey(recv_buffer, &rkey);
      rb = (uint64_t*) rb_ptr;
      memset(rb, 0, YMPI_PAGE_SIZE + total_io * sizeof(int64_t));
      MPI_Send(&rb, 1, MPI_UNSIGNED_LONG_LONG, 0, MPI_TAG, MPI_COMM_WORLD);
      MPI_Send(&rkey, 1, MPI_UNSIGNED, 0, MPI_TAG, MPI_COMM_WORLD);
      volatile uint64_t* sig = &rb[0];
      LOGD("  wait for synchronization message at sig=%p\n", sig);
      {
        while(*sig == 0);
      }
      LOGD("  reveived signal %llu\n", *sig);
      rb = &rb[YMPI_PAGE_SIZE / sizeof(int64_t)];
      int err = 0;

      for(i=0; i<RDMA_NUM_ELEM; i++) {
        if(rb[i] != 0x1111111111111111 + i) {
          printf("!!! ERROR at rb[%d]:  expected %llu   actual %llu\n", i, 0x1111111111111111 + i, rb[i]);
          err = 1;
        }
      }
      if(err) {
        assert(0);
      } else {
        LOGD("  CORRECTNESS PASSED...\n");
      }
    }
  }
  YMPI_Zflush();
  MPI_Barrier(MPI_COMM_WORLD);

  {
    /* Batched RDMA
       By Batched RDMA, we mean that multiple RDMA operations are
       issued before waiting for completion
     */
    int total_io = 8*1024*1024;
    int batch_size = 4;
    int j,k;
    LOGDS("Testing Batched RDMA Write correctness (%d packet, batch_size=%d)...\n", total_io, batch_size);
    if(rank == 0) {
      YMPI_Rdma_buffer send_buffer;
      uint64_t* sb   = NULL;
      uint32_t  rkey = 0;
      uint64_t* rb   = NULL;
      uintptr_t sb_ptr = 0;
      YMPI_Allocate(&send_buffer, total_io * sizeof(int64_t), YMPI_BUFFER_TYPE_REMOTE_ATOMIC);
      YMPI_Get_buffer(send_buffer, &sb_ptr);
      sb = (uint64_t*) sb_ptr;
      MPI_Recv(&rb, 1, MPI_UNSIGNED_LONG_LONG, 1, MPI_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      MPI_Recv(&rkey, 1, MPI_UNSIGNED, 1, MPI_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      assert(sb != NULL);
      assert(rb != NULL);

      uint64_t* sig = &rb[0];
      rb = &rb[YMPI_PAGE_SIZE / sizeof(int64_t)];
      for(i=0; i<total_io; i++) {
        sb[i] = 0x1111111111111111 + i;
      }
      double wsec = -MPI_Wtime();
      for(i=0; i<total_io; i+=batch_size) {
        for(j=i; j<(i+batch_size); j++) {
          YMPI_Write(send_buffer, j * sizeof(uint64_t), sizeof(uint64_t), 1, rkey, &rb[j]);
        }
        YMPI_Zflush();
      }
      wsec += MPI_Wtime();
      LOGD("  time=%lf us,  msg_rate=%lf Mmesg/s\n", wsec, 1e-6 * total_io / wsec);
      YMPI_Write(send_buffer, 0, sizeof(uint64_t), 1, rkey, sig);
      YMPI_Dealloc(&send_buffer);
    } else if(rank == 1) {
      YMPI_Rdma_buffer recv_buffer;
      uint32_t  rkey = 0;
      uint64_t* rb = NULL;
      uintptr_t rb_ptr = 0;
      YMPI_Allocate(&recv_buffer, YMPI_PAGE_SIZE + total_io * sizeof(int64_t), YMPI_BUFFER_TYPE_REMOTE_ATOMIC);
      YMPI_Get_buffer(recv_buffer, &rb_ptr);
      YMPI_Get_rkey(recv_buffer, &rkey);
      rb = (uint64_t*) rb_ptr;
      memset(rb, 0, YMPI_PAGE_SIZE + total_io * sizeof(int64_t));
      MPI_Send(&rb, 1, MPI_UNSIGNED_LONG_LONG, 0, MPI_TAG, MPI_COMM_WORLD);
      MPI_Send(&rkey, 1, MPI_UNSIGNED, 0, MPI_TAG, MPI_COMM_WORLD);
      volatile uint64_t* sig = &rb[0];
      LOGD("  wait for synchronization message at sig=%p\n", sig);
      {
        while(*sig == 0);
      }
      LOGD("  reveived signal %llu\n", *sig);
      rb = &rb[YMPI_PAGE_SIZE / sizeof(int64_t)];
      int err = 0;

      for(i=0; i<RDMA_NUM_ELEM; i++) {
        if(rb[i] != 0x1111111111111111 + i) {
          printf("!!! ERROR at rb[%d]:  expected %llu   actual %llu\n", i, 0x1111111111111111 + i, rb[i]);
          err = 1;
        }
      }
      if(err) {
        assert(0);
      } else {
        LOGD("  CORRECTNESS PASSED...\n");
      }
    }
  }
  YMPI_Zflush();
  MPI_Barrier(MPI_COMM_WORLD);

  {
    int total_io = 8*1024*1024;
    int batch_size = 8;
    int j,k;
    LOGDS("Testing Batched RDMA Write correctness (%d packet, batch_size=%d)...\n", total_io, batch_size);
    if(rank == 0) {
      YMPI_Rdma_buffer send_buffer;
      uint64_t* sb   = NULL;
      uint32_t  rkey = 0;
      uint64_t* rb   = NULL;
      uintptr_t sb_ptr = 0;
      YMPI_Allocate(&send_buffer, total_io * sizeof(int64_t), YMPI_BUFFER_TYPE_REMOTE_ATOMIC);
      YMPI_Get_buffer(send_buffer, &sb_ptr);
      sb = (uint64_t*) sb_ptr;
      MPI_Recv(&rb, 1, MPI_UNSIGNED_LONG_LONG, 1, MPI_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      MPI_Recv(&rkey, 1, MPI_UNSIGNED, 1, MPI_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      assert(sb != NULL);
      assert(rb != NULL);

      uint64_t* sig = &rb[0];
      rb = &rb[YMPI_PAGE_SIZE / sizeof(int64_t)];
      for(i=0; i<total_io; i++) {
        sb[i] = 0x1111111111111111 + i;
      }
      double wsec = -MPI_Wtime();
      for(i=0; i<total_io; i+=batch_size) {
        for(j=i; j<(i+batch_size); j++) {
          YMPI_Write(send_buffer, j * sizeof(uint64_t), sizeof(uint64_t), 1, rkey, &rb[j]);
        }
        YMPI_Zflush();
      }
      wsec += MPI_Wtime();
      LOGD("  time=%lf us,  msg_rate=%lf Mmesg/s\n", wsec, 1e-6 * total_io / wsec);
      YMPI_Write(send_buffer, 0, sizeof(uint64_t), 1, rkey, sig);
      YMPI_Dealloc(&send_buffer);
    } else if(rank == 1) {
      YMPI_Rdma_buffer recv_buffer;
      uint32_t  rkey = 0;
      uint64_t* rb = NULL;
      uintptr_t rb_ptr = 0;
      YMPI_Allocate(&recv_buffer, YMPI_PAGE_SIZE + total_io * sizeof(int64_t), YMPI_BUFFER_TYPE_REMOTE_ATOMIC);
      YMPI_Get_buffer(recv_buffer, &rb_ptr);
      YMPI_Get_rkey(recv_buffer, &rkey);
      rb = (uint64_t*) rb_ptr;
      memset(rb, 0, YMPI_PAGE_SIZE + total_io * sizeof(int64_t));
      MPI_Send(&rb, 1, MPI_UNSIGNED_LONG_LONG, 0, MPI_TAG, MPI_COMM_WORLD);
      MPI_Send(&rkey, 1, MPI_UNSIGNED, 0, MPI_TAG, MPI_COMM_WORLD);
      volatile uint64_t* sig = &rb[0];
      LOGD("  wait for synchronization message at sig=%p\n", sig);
      {
        while(*sig == 0);
      }
      LOGD("  reveived signal %llu\n", *sig);
      rb = &rb[YMPI_PAGE_SIZE / sizeof(int64_t)];
      int err = 0;

      for(i=0; i<RDMA_NUM_ELEM; i++) {
        if(rb[i] != 0x1111111111111111 + i) {
          printf("!!! ERROR at rb[%d]:  expected %llu   actual %llu\n", i, 0x1111111111111111 + i, rb[i]);
          err = 1;
        }
      }
      if(err) {
        assert(0);
      } else {
        LOGD("  CORRECTNESS PASSED...\n");
      }
    }
  }
  YMPI_Zflush();
  MPI_Barrier(MPI_COMM_WORLD);

  {
    int total_io = 8*1024*1024;
    LOGDS("Benchmarking RDMA Read IOPS (%d packet)\n", total_io);
    if(rank == 0) {
      int i;
      YMPI_Rdma_buffer send_buffer;
      uint64_t* sb = NULL;
      uintptr_t sb_ptr = 0;
      uint32_t  sb_rkey = 0;
      YMPI_Allocate(&send_buffer, total_io * sizeof(uint64_t), YMPI_BUFFER_TYPE_REMOTE_ATOMIC);
      YMPI_Get_buffer(send_buffer, &sb_ptr);
      sb = (uint64_t*) sb_ptr;
      memset(sb, 0, total_io * sizeof(uint64_t));
      for(i=0; i<total_io; i++) {
        sb[i] = 0x1111111111111111 + i;
      }
      YMPI_Get_rkey(send_buffer, &sb_rkey);
      LOGD("  sb=%p  sb_rkey=%u\n", sb, sb_rkey);
      MPI_Send(&sb, 1, MPI_UNSIGNED_LONG_LONG, 1, MPI_TAG, MPI_COMM_WORLD);
      MPI_Send(&sb_rkey, 1, MPI_UNSIGNED, 1, MPI_TAG, MPI_COMM_WORLD);
      MPI_Barrier(MPI_COMM_WORLD);
      YMPI_Dealloc(&send_buffer);
    } else if (rank == 1) {
      int i;
      YMPI_Rdma_buffer recv_buffer;
      uint64_t* sb;
      volatile uint64_t* rb;
      uintptr_t rb_ptr;
      uint32_t rkey;
      YMPI_Allocate(&recv_buffer, total_io * sizeof(uint64_t), YMPI_BUFFER_TYPE_REMOTE_ATOMIC);
      YMPI_Get_buffer(recv_buffer, &rb_ptr);
      rb = (uint64_t*) rb_ptr;
      MPI_Recv(&sb, 1, MPI_UNSIGNED_LONG_LONG, 0, MPI_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      MPI_Recv(&rkey, 1, MPI_UNSIGNED, 0, MPI_TAG, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
      LOGD("  start reading:  sb=%p   rkey=%u\n", sb, rkey);
      double wsec = -MPI_Wtime();
      for(i=0; i<total_io; i++) {
        // LOGD("Read sb[%llu] from %d\n", i*sizeof(uint64_t), 0);
        YMPI_Read(recv_buffer, i * sizeof(uint64_t), sizeof(uint64_t), 0, rkey, &sb[i]);
      }
      YMPI_Zflush();
      wsec += MPI_Wtime();
      LOGD("  time=%lf us,  msg_rate=%lf Mmesg/s\n", wsec, 1e-6 * total_io / wsec);

      int err = 0;
      for(i=0; i<total_io; i++) {
        if(rb[i] != 0x1111111111111111 + i) {
          LOGD("!!! ERROR rb[%d]    expected %llu   actual %llu\n", i, 0x1111111111111111 + i, rb[i]);
          err = 1;
        }
      }
      if(err) {
        exit(-1);
      } else {
        LOGD("  CORRECTNESS PASSED...\n");
      }
      MPI_Barrier(MPI_COMM_WORLD);
      YMPI_Dealloc(&recv_buffer);
    }
  }
  MPI_Barrier(MPI_COMM_WORLD);

  {
    LOGDS("Benchmarking message rate (%d iters, %d mesg/iter, tx_depth=%d)...\n", iters, YMPI_MAX_SEND_WR_PER_QP, YMPI_MAX_SEND_WR_PER_QP);
    double wsec = 0.0;
    int k;
    if(rank == 0) {
      YMPI_Rdma_buffer send_buffer;
      uint64_t* sb = NULL;
      uintptr_t sb_ptr = 0;
      YMPI_Alloc(&send_buffer, YMPI_MAX_SEND_WR_PER_QP * sizeof(int64_t));
      YMPI_Get_buffer(send_buffer, &sb_ptr);
      sb = (uint64_t*) sb_ptr;
      assert(sb != NULL);

      wsec = -MPI_Wtime();
      for(k=0; k<iters; k++) {
        for(i=0; i<YMPI_MAX_SEND_WR_PER_QP; i++) {
          sb[i] = 0x1111111111111111 + i;
          YMPI_Zsend(send_buffer, i * sizeof(uint64_t), sizeof(uint64_t), 1);
          // LOGD("Zsend sb[%llu] to %d\n", i*sizeof(uint64_t), 1);
        }
        YMPI_Zflush();
      }
      wsec += MPI_Wtime();

      YMPI_Dealloc(&send_buffer);
    } else if(rank == 1) {
      uint64_t* recv_buffers[YMPI_MAX_SEND_WR_PER_QP];
      uint64_t  recv_buffers_len[YMPI_MAX_SEND_WR_PER_QP];
      memset(recv_buffers, 0, sizeof(recv_buffers));
      memset(recv_buffers_len, 0, sizeof(recv_buffers_len));

      wsec = -MPI_Wtime();
      for(k=0; k<iters; k++) {
        for(i=0; i<YMPI_MAX_SEND_WR_PER_QP; i++) {
          YMPI_Zrecv(&recv_buffers[i], &recv_buffers_len[i], 0);
          // LOGD("Zrecv rb[%llu] from %d: len=%d\n", i*sizeof(uint64_t), 0, recv_buffers_len[i]);
        }
        YMPI_Return();
      }
      wsec += MPI_Wtime();
    }
    MPI_Barrier(MPI_COMM_WORLD);
    LOGD("time=%lf us,  msg_rate=%lf Mmesg/s\n", wsec, 1e-6 * iters * YMPI_MAX_SEND_WR_PER_QP / wsec);
  }

  MPI_Barrier(MPI_COMM_WORLD);

  {
    LOGDS("Testing point-to-point zero-bytes correctness (%d packet)...\n", YMPI_MAX_SEND_WR_PER_QP);
    if(rank == 0) {
      YMPI_Rdma_buffer send_buffer;
      uint64_t* sb = NULL;
      uintptr_t sb_ptr = 0;
      YMPI_Alloc(&send_buffer, YMPI_MAX_SEND_WR_PER_QP * sizeof(int64_t));
      YMPI_Get_buffer(send_buffer, &sb_ptr);
      sb = (uint64_t*) sb_ptr;
      assert(sb != NULL);
      for(i=0; i<YMPI_MAX_SEND_WR_PER_QP; i++) {
        sb[i] = 0x1111111111111111 + i;
        YMPI_Zsend(send_buffer, i * sizeof(uint64_t), 0, 1);
      }
      YMPI_Zflush();
      YMPI_Dealloc(&send_buffer);
    } else if(rank == 1) {
      uint64_t* recv_buffers[YMPI_MAX_SEND_WR_PER_QP];
      uint64_t  recv_buffers_len[YMPI_MAX_SEND_WR_PER_QP];
      memset(recv_buffers, 0, sizeof(recv_buffers));
      memset(recv_buffers_len, 0, sizeof(recv_buffers_len));

      for(i=0; i<YMPI_MAX_SEND_WR_PER_QP; i++) {
        YMPI_Zrecv(&recv_buffers[i], &recv_buffers_len[i], 0);
        // printf("recv_buffers_len[%d]  = %llu\n", i, recv_buffers_len[i]);
        assert(recv_buffers_len[i] == 0);
        // printf("recv_buffers[%d]    = %p [%llu]\n", i, recv_buffers[i], *(recv_buffers[i]));
      }
      YMPI_Return();
    }
  }

  YMPI_Finalize();
  MPI_Finalize();
  return 0;
}
