CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -O2 -D_GNU_SOURCE
LDFLAGS =

SRC_DIR   = src
BUILD_DIR = build

TARGET  = $(BUILD_DIR)/sim
SRCS    = $(wildcard $(SRC_DIR)/*.c)
OBJS    = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS    = $(SRC_DIR)/sim.h

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(DEPS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: all
	./$(TARGET)

gen: all
	./$(TARGET) -s edf -t 80

fail: all
	./$(TARGET) -f 0.05 -t 100

clean:
	rm -rf $(BUILD_DIR) log.log

rebuild: clean all

.PHONY: all run gen fail clean rebuild
