.PHONY: all clean

SW5CC_PATH := $(shell which sw5cc.new 2>/dev/null)

ifeq ($(SW5CC_PATH),)
  $(info Compiling in x86_64 environment)
  MPICC := mpicc
  CFLAGS  := -DX86 -Wall -O3 -g -Iinclude
  LDFLAGS := -libverbs
else
  $(info Compiling in sw environment)
  MPICC := mpicc
  CFLAGS  := -Wall -O3 -OPT:IEEE_arith=1 -Iinclude -I/usr/sw-mpp/include
  LDFLAGS :=
  # LDLIBS  := ${LDLIBS} -L/usr/sw-mpp/lib -lrdmacm -libverbs -lpthread
  # LDLIBS  := -hybrid ${LDLIBS} -DCONFIG_SWIB -L/usr/sw-mpp/lib /usr/sw-mpp/mpi2/lib/__slave_dma.o -L/usr/sw-mpp/mpi2/lib -lmpi -lswtm -libverbs_wd -Wl,-zmuldefs -lrt -ldl -lpthread -lm -los_master_isp
endif

TARGET := src/ibprobe src/ibstat src/iballputall test/test_linkedlist #src/devinfo

all: $(TARGET)

src/ibprobe: src/ibprobe.c
	$(MPICC) $(LDFLAGS) -o src/ibprobe src/ibprobe.c -I/usr/sw-mpp/include

src/ibstat: src/ibstat.c
	$(MPICC) $(LDFLAGS) -o src/ibstat src/ibstat.c -I/usr/sw-mpp/include

src/iballputall: src/iballputall.c include/common.h
	$(MPICC) $(CFLAGS) $(LDFLAGS) -o src/iballputall src/iballputall.c -I/usr/sw-mpp/include -Iinclude

src/ympi.o: src/ympi.c include/ympi.h include/common.h
	$(MPICC) -c $(CFLAGS) -o src/ympi.o src/ympi.c

test/benchmark_ympi: test/benchmark_ympi.c src/ympi.c include/ympi.h include/common.h
	$(MPICC) $(CFLAGS) $(LDFLAGS) -o test/benchmark_ympi -I/usr/sw-mpp/include -Iinclude test/benchmark_ympi.c src/ympi.c

test/benchmark_mpi: test/benchmark_mpi.c
	$(MPICC) $(CFLAGS) $(LDFLAGS) -o test/benchmark_mpi test/benchmark_mpi.c

test/test_linkedlist: test/test_linkedlist.c include/linkedlist.h
	gcc -g -o test/test_linkedlist -Iinclude test/test_linkedlist.c

test/test_ympi: test/test_ympi.c src/ympi.c include/ympi.h include/common.h
	$(MPICC) $(CFLAGS) $(LDFLAGS) -o test/test_ympi -I/usr/sw-mpp/include -Iinclude test/test_ympi.c src/ympi.c

test/test_ympi_coll: test/test_ympi_coll.c src/ympi.c include/ympi.h include/common.h
	$(MPICC) $(CFLAGS) $(LDFLAGS) -o test/test_ympi_coll -I/usr/sw-mpp/include -Iinclude test/test_ympi_coll.c src/ympi.c

test/test_ympi.x86: test/test_ympi.c src/ympi.c include/ympi.h include/common.h
	$(MPICC) -L/usr/sw-cluster/slurm-16.05.3/lib $(CFLAGS_X86) -o test/test_ympi.x86 test/test_ympi.c src/ympi.c

benchmark/ympi_latency: benchmark/ympi_latency.c src/ympi.c include/ympi.h include/common.h
	$(MPICC) $(CFLAGS) -o benchmark/ympi_latency -I/usr/sw-mpp/include -Iinclude benchmark/ympi_latency.c src/ympi.c

benchmark/ympi_fan: benchmark/ympi_fan.c src/ympi.c include/ympi.h include/common.h
	$(MPICC) $(CFLAGS) $(LDFLAGS) -o benchmark/ympi_fan -I/usr/sw-mpp/include -Iinclude benchmark/ympi_fan.c src/ympi.c

benchmark/ympi_allputall: benchmark/ympi_allputall.c src/ympi.c include/ympi.h include/common.h
	$(MPICC) $(CFLAGS) $(LDFLAGS) -o benchmark/ympi_allputall -I/usr/sw-mpp/include -Iinclude benchmark/ympi_allputall.c src/ympi.c

clean:
	rm -f $(TARGET)

