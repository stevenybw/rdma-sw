.PHONY: clean

CC := sw5cc.new
CFLAGS  := -host -Wall -g -I/usr/sw-mpp/include -I/usr/sw-mpp/mpi2/include
#LDLIBS  := ${LDLIBS} -L/usr/sw-mpp/lib -lrdmacm -libverbs -lpthread
LDLIBS  := -hybrid ${LDLIBS} -DCONFIG_SWIB -L/usr/sw-mpp/lib /usr/sw-mpp/mpi2/lib/__slave_dma.o -L/usr/sw-mpp/mpi2/lib -lmpi -lswtm -libverbs_wd -Wl,-zmuldefs -lrt -ldl -lpthread -lm -los_master_isp
EXEC := ibprobe

TARGET := src/ibprobe #src/devinfo

src/ibprobe: src/ibprobe.c
	mpicc -o src/ibprobe src/ibprobe.c -I/usr/sw-mpp/include

src/ibstat: src/ibstat.c
	mpicc -o src/ibstat src/ibstat.c -I/usr/sw-mpp/include

all: $(TARGET)

clean:
	rm -f $(TARGET)

