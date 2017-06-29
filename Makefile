.PHONY: all clean

TARGET := src/ibprobe src/ibstat src/iballputall #src/devinfo

all: $(TARGET)

CC := sw5cc.new
CFLAGS  := -Wall -O3 -OPT:IEEE_arith=1
# LDLIBS  := ${LDLIBS} -L/usr/sw-mpp/lib -lrdmacm -libverbs -lpthread
# LDLIBS  := -hybrid ${LDLIBS} -DCONFIG_SWIB -L/usr/sw-mpp/lib /usr/sw-mpp/mpi2/lib/__slave_dma.o -L/usr/sw-mpp/mpi2/lib -lmpi -lswtm -libverbs_wd -Wl,-zmuldefs -lrt -ldl -lpthread -lm -los_master_isp

src/ibprobe: src/ibprobe.c
	mpicc -o src/ibprobe src/ibprobe.c -I/usr/sw-mpp/include

src/ibstat: src/ibstat.c
	mpicc -o src/ibstat src/ibstat.c -I/usr/sw-mpp/include

src/iballputall: src/iballputall.c include/common.h
	mpicc $(CFLAGS) -o src/iballputall src/iballputall.c -I/usr/sw-mpp/include -Iinclude

test/test_ympi: test/test_ympi.c src/ympi.c include/ympi.h include/common.h
	mpicc $(CFLAGS) -o test/test_ympi -I/usr/sw-mpp/include -Iinclude test/test_ympi.c src/ympi.c

clean:
	rm -f $(TARGET)

