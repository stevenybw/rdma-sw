#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <mpi.h>

#include "common.h"
#include "ympi.h"

int main(void) {
	int i, rank, nprocs;
	YMPI_Init(NULL, NULL);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
	LOGDS("Testing YMPI...\n");

	if(rank == 0) {
		YMPI_Rdma_buffer send_buffer;
		uint64_t* sb = NULL;
                uintptr_t sb_ptr = 0;
		YMPI_Alloc(&send_buffer, 1024);
		YMPI_Get_buffer(send_buffer, &sb_ptr);
                sb = (uint64_t*) sb_ptr;
		assert(sb != NULL);
		for(i=0; i<128; i++) {
			sb[i] = i;
		}
		YMPI_Post_send(send_buffer, 0, 1024, 1);
		YMPI_Expect(1, 0, NULL, NULL);
		YMPI_Dealloc(&send_buffer);
	} else if(rank == 1) {
		uint64_t* recv_buffer = NULL;
		uint64_t  recv_buffer_len = 0;
		YMPI_Expect(0, 1, &recv_buffer, &recv_buffer_len);
                printf("recv_buffer     = %p\n", recv_buffer);
                printf("recv_buffer_len = %d\n", recv_buffer_len);
		assert(recv_buffer_len == 1024);
		for(i=0; i<128; i++) {
			assert(recv_buffer[i] == i);
		}
	}

	YMPI_Finalize();
	return 0;
}

