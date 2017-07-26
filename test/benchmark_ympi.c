#include <stdio.h>
#include <string.h>
#include "ympi.h"

int main(void)
{
	YMPI_Init(NULL, NULL);

	int np, nb;
	int rank, nprocs, bytes=128*1024, iter=32, skip=8;

	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

	void* sb;
	uintptr_t sb_ptr = 0;
	YMPI_Rdma_buffer send_buffer;
	YMPI_Alloc(&send_buffer, nprocs * bytes);
	YMPI_Get_buffer(send_buffer, &sb_ptr);
	sb = (void*) sb_ptr;
	memset(sb, 0, nprocs * bytes);

	for(np=2; np<=nprocs; np*=2) {
		int np_mask = np-1;
		for(nb=32; nb<=bytes; nb*=2) {
			MPI_Barrier(MPI_COMM_WORLD);
			if(rank < np) {
				int q;
				double bt = MPI_Wtime();
				int i;
				for(i=0; i<iter; i++) {
					if(i==skip) {
						bt = MPI_Wtime();
					}
					for(q=(rank+1)&np_mask; q!=rank; q=(q+1)&mask) {
						YMPI_Zsend(send_buffer, q*nb, nb, q);
					}
					YMPI_Zflush();
					for(q=(rank+1)&np_mask; q!=rank; q=(q+1)&mask) {
						uint64_t ptr, len;
						YMPI_Zrecv(&ptr, &len, q);
					}
				}
				double duration = MPI_Wtime() - bt;
				printf("%d procs   %d bytes   %.2lf us\n", np, nb, 1e6*duration/(iter-skip));
			}
			MPI_Barrier(MPI_COMM_WORLD);
			if(rank < np) {
				YMPI_Return();
			}
			MPI_Barrier(MPI_COMM_WORLD);
		}
	}

	YMPI_Finalize();
	return 0;
}