#ifndef YMPI_H
#define YMPI_H

#include <stdint.h>

#define YMPI_PAGE_SIZE        (1024*1024)
#define YMPI_VBUF_BYTES       (2*1024*1024)
#define YMPI_PREPOST_DEPTH    (128)
#define YMPI_QPN_HASH_SIZE    (1024*1024)

// affect pending receive linked list for each process
#define YMPI_RECV_PER_PROCESS (1024)

#define YMPI_SW               (1)
#define YMPI_SW_DIY           (YMPI_SW)

typedef uint64_t YMPI_Rdma_buffer;
typedef uint64_t uintptr_t;

int YMPI_Init(int *argc, char ***argv);
int YMPI_Init_ranklist(int *argc, char ***argv, int* target_rank_list);

int YMPI_Finalize();

int YMPI_Alloc(YMPI_Rdma_buffer *buffer, size_t bytes);
int YMPI_Dealloc(YMPI_Rdma_buffer *buffer);
int YMPI_Get_buffer(YMPI_Rdma_buffer buffer, uintptr_t* buf);

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

#endif

