CC = gcc
CFLAGS = -Wall -std=c99 -g
LDLIBS = -lcurl -lz
LDFLAGS = -g -pthread

SRCS = paster.c ./starter/png_util/lab_png.c ./starter/png_util/crc.c ./starter/png_util/zutil.c
OBJS = $(SRCS:.c=.o)
TARGET = paster

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDLIBS) $(LDFLAGS) 

%.o: %.c
	$(CC) -c $< -o $@ $(CFLAGS)

clean:
	rm -f $(OBJS) $(TARGET)