#ifndef YMPI_H
#define YMPI_H

#include <stdint.h>

#define YMPI_PAGE_SIZE          (1024*1024)
#define YMPI_VBUF_BYTES         (2*1024*1024)
#define YMPI_PREPOST_DEPTH      (128)
#define YMPI_MAX_SEND_WR_PER_QP (8)
#define YMPI_QPN_HASH_SIZE      (1024*1024)

// affect pending receive linked list for each process
#define YMPI_RECV_PER_PROCESS (1024)

#if   defined(YMPI_ARCH_SW)
  #warning Compiling in SW architecture
  #define YMPI_SW_DIY           1
#elif defined(YMPI_ARCH_X86_64)
  #warning Compiling in x86_64 architecture
#else
  #error   You should specify the architecture
#endif

typedef uint64_t YMPI_Rdma_buffer;
typedef uint64_t uintptr_t;

int YMPI_Init(int *argc, char ***argv);
int YMPI_Init_ranklist(int *argc, char ***argv, int* target_rank_list);

int YMPI_Finalize();

// allocate a buffer at least bytes, return the handle via pointer
#define YMPI_BUFFER_TYPE_LOCAL         0
#define YMPI_BUFFER_TYPE_REMOTE_RO     1
#define YMPI_BUFFER_TYPE_REMOTE_RW     2
#define YMPI_BUFFER_TYPE_REMOTE_ATOMIC 3
int YMPI_Allocate(YMPI_Rdma_buffer *buffer, size_t bytes, int buffer_type);

// backward compability: allocate local buffer
int YMPI_Alloc(YMPI_Rdma_buffer *buffer, size_t bytes);
int YMPI_Dealloc(YMPI_Rdma_buffer *buffer);
int YMPI_Get_buffer(YMPI_Rdma_buffer buffer, uintptr_t* buf);

/* Part One: Zero Copy Messaging */

// post the **buffer** with specific **length of bytes** to **dest**
int YMPI_Zsend(YMPI_Rdma_buffer buffer, size_t offset, size_t bytes, int dest);

// receive one message from the source
int YMPI_Zrecv(void** recv_buffer_ptr, uint64_t* recv_buffer_len_ptr, int source);

// wait for exactly **num_message** messages, return the array of pointers, and the length for each message by argument from any sources.
int YMPI_Zrecvany(int num_message, void* recv_buffers[], uint64_t recv_buffers_len[]);

// flush pending send
int YMPI_Zflush();

// return all the received buffer to YMPI
int YMPI_Return();


/* Part Two: RDMA Operations */

int YMPI_RDMA_Write(YMPI_Rdma_buffer local_src, size_t offset, size_t bytes, int dest, uint64_t dest_ptr);
int YMPI_RDMA_Read (YMPI_Rdma_buffer local_dst, size_t offset, size_t* bytes, int src, uint64_t src_ptr);

#endif

