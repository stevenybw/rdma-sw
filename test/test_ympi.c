#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <mpi.h>

#include "common.h"
#include "ympi.h"

int main(void) {
	YMPI_Init(NULL, NULL);

	LOGDS("Testing YMPI...\n");

	YMPI_Finalize();
	return 0;
}

