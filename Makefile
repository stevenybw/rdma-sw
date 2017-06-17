.PHONY: clean

CC := sw5cc.new
CFLAGS  := -host -Wall -g -I/usr/sw-mpp/include
LDLIBS  := ${LDLIBS} -L/usr/sw-mpp/lib -lrdmacm -libverbs -lpthread
EXEC := ibprobe

TARGET := src/ibprobe src/devinfo

all: $(TARGET)

clean:
	rm -f $(TARGET)

