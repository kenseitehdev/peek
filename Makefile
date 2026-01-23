CC = clang
CFLAGS = -Wall -Wextra -std=c11
LDFLAGS = -lncurses

SRC_DIR = src
BUILD_DIR = build
BIN_DIR = bin

SRC = $(SRC_DIR)/main.c
OBJ = $(BUILD_DIR)/main.o
TARGET = $(BIN_DIR)/peek

all: $(TARGET)

$(TARGET): $(OBJ) | $(BIN_DIR)
	$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS)

$(OBJ): $(SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $(SRC) -o $(OBJ)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/peek
	@echo "Installed peek to /usr/local/bin/"

uninstall:
	rm -f /usr/local/bin/peek
	@echo "Uninstalled peek"

.PHONY: all clean install uninstall
