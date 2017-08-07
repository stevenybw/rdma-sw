.PHONY: all clean

TARGET := src/ibprobe src/ibstat src/iballputall test/test_linkedlist #src/devinfo

all: $(TARGET)

CC := sw5cc.new
CFLAGS  := -Wall -O3 -OPT:IEEE_arith=1 -Iinclude -I/usr/sw-mpp/include
CFLAGS_X86  := -DX86 -Wall -O3 -g -Iinclude
# LDLIBS  := ${LDLIBS} -L/usr/sw-mpp/lib -lrdmacm -libverbs -lpthread
# LDLIBS  := -hybrid ${LDLIBS} -DCONFIG_SWIB -L/usr/sw-mpp/lib /usr/sw-mpp/mpi2/lib/__slave_dma.o -L/usr/sw-mpp/mpi2/lib -lmpi -lswtm -libverbs_wd -Wl,-zmuldefs -lrt -ldl -lpthread -lm -los_master_isp

src/ibprobe: src/ibprobe.c
	mpicc -o src/ibprobe src/ibprobe.c -I/usr/sw-mpp/include

src/ibstat: src/ibstat.c
	mpicc -o src/ibstat src/ibstat.c -I/usr/sw-mpp/include

src/iballputall: src/iballputall.c include/common.h
	mpicc $(CFLAGS) -o src/iballputall src/iballputall.c -I/usr/sw-mpp/include -Iinclude

src/ympi.o: src/ympi.c include/ympi.h include/common.h
	mpicc -c $(CFLAGS) -o src/ympi.o src/ympi.c

test/benchmark_ympi: test/benchmark_ympi.c src/ympi.c include/ympi.h include/common.h
	mpicc $(CFLAGS) -o test/benchmark_ympi -I/usr/sw-mpp/include -Iinclude test/benchmark_ympi.c src/ympi.c

test/benchmark_mpi: test/benchmark_mpi.c
	mpicc $(CFLAGS) -o test/benchmark_mpi test/benchmark_mpi.c

test/test_linkedlist: test/test_linkedlist.c include/linkedlist.h
	gcc -g -o test/test_linkedlist -Iinclude test/test_linkedlist.c

test/test_ympi: test/test_ympi.c src/ympi.c include/ympi.h include/common.h
	mpicc $(CFLAGS) -o test/test_ympi -I/usr/sw-mpp/include -Iinclude test/test_ympi.c src/ympi.c

test/test_ympi_coll: test/test_ympi_coll.c src/ympi.c include/ympi.h include/common.h
	mpicc $(CFLAGS) -o test/test_ympi_coll -I/usr/sw-mpp/include -Iinclude test/test_ympi_coll.c src/ympi.c

test/test_ympi.x86: test/test_ympi.c src/ympi.c include/ympi.h include/common.h
	/usr/sw-mpp/mpi2_x86/bin/mpicc -L/usr/sw-cluster/slurm-16.05.3/lib $(CFLAGS_X86) -o test/test_ympi.x86 test/test_ympi.c src/ympi.c

clean:
	rm -f $(TARGET)

