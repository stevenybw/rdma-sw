#ifndef YMPI_H
#define YMPI_H

#include <stdint.h>

#define YMPI_PAGE_SIZE        (1024*1024)
#define YMPI_VBUF_BYTES       (256*1024)
#define YMPI_PREPOST_DEPTH    (32)  

typedef uint64_t YMPI_Rdma_buffer;
typedef uint64_t uintptr_t;

int YMPI_Init(int *argc, char ***argv);
int YMPI_Finalize();

int YMPI_Alloc(YMPI_Rdma_buffer *buffer, size_t bytes);
int YMPI_Dealloc(YMPI_Rdma_buffer *buffer);
int YMPI_Get_buffer(YMPI_Rdma_buffer buffer, uintptr_t* buf);

// post the **buffer** with specific **length of bytes** to **dest**
int YMPI_Post_send(YMPI_Rdma_buffer buffer, size_t bytes, int dest);

// wait for exactly **num_message** messages, return the array of pointers, and the length for each message by argument.
int YMPI_Expect(int num_message, void* recv_buffers[], uint64_t recv_buffers_len[]);

// return the buffer to the window
int YMPI_Return(int num_message, void* recv_buffers[]);

#endif

