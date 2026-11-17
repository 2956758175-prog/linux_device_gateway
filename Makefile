
# ==========================================
# Compiler Configuration
# ==========================================

CC := gcc

# 普通编译选项
CFLAGS := -Wall -Wextra -O2 -g

# Sanitizer 编译选项
SANITIZER_FLAGS := -Wall -Wextra -O1 -g \
                   -fsanitize=address,undefined \
                   -fno-omit-frame-pointer

# 头文件目录
CPPFLAGS := -Iinclude

# 输出目录
BUILD_DIR := build


# ==========================================
# Source Files
# ==========================================

# 服务端源文件
SERVER_SRCS := \
	src/server.c \
	src/protocol.c \
	src/protocol_parser.c \
	src/device_manager.c \
	src/logger.c

# 普通客户端源文件
CLIENT_SRCS := \
	src/client.c \
	src/protocol.c

# 第二客户端源文件
CLIENT2_SRCS := \
	src/client2.c \
	src/protocol.c

# 协议解析测试源文件
PARSER_TEST_SRCS := \
	src/test_parser.c \
	src/protocol.c \
	src/protocol_parser.c

# 多客户端压力测试源文件
CLIENT_LOAD_SRCS := \
	src/client_load.c \
	src/protocol.c

# 协议解析模糊测试源文件
FUZZ_TEST_SRCS := \
	src/fuzz_parser.c \
	src/protocol.c \
	src/protocol_parser.c


# ==========================================
# Default Target
# ==========================================

.PHONY: all

all: $(BUILD_DIR)/server \
     $(BUILD_DIR)/client \
     $(BUILD_DIR)/client2 \
     $(BUILD_DIR)/test_parser \
     $(BUILD_DIR)/client_load \
     $(BUILD_DIR)/fuzz_parser


# ==========================================
# Build Directory
# ==========================================

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)


# ==========================================
# Normal Server
# ==========================================

$(BUILD_DIR)/server: $(SERVER_SRCS) $(wildcard include/*.h) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SERVER_SRCS) -o $@


# ==========================================
# ASan + UBSan Server
# ==========================================

$(BUILD_DIR)/server_asan: $(SERVER_SRCS) $(wildcard include/*.h) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(SANITIZER_FLAGS) $(SERVER_SRCS) -o $@


.PHONY: asan

asan: $(BUILD_DIR)/server_asan


# ==========================================
# Normal Client
# ==========================================

$(BUILD_DIR)/client: $(CLIENT_SRCS) $(wildcard include/*.h) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CLIENT_SRCS) -o $@


# ==========================================
# Second Client
# ==========================================

$(BUILD_DIR)/client2: $(CLIENT2_SRCS) $(wildcard include/*.h) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CLIENT2_SRCS) -o $@


# ==========================================
# Protocol Parser Test
# ==========================================

$(BUILD_DIR)/test_parser: $(PARSER_TEST_SRCS) $(wildcard include/*.h) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PARSER_TEST_SRCS) -o $@


.PHONY: test

test: $(BUILD_DIR)/test_parser
	./$(BUILD_DIR)/test_parser


# ==========================================
# Multi-client Stress Test
# ==========================================

$(BUILD_DIR)/client_load: $(CLIENT_LOAD_SRCS) $(wildcard include/*.h) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CLIENT_LOAD_SRCS) -o $@


# ==========================================
# Protocol Parser Fuzz Test
# ==========================================

$(BUILD_DIR)/fuzz_parser: $(FUZZ_TEST_SRCS) $(wildcard include/*.h) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(SANITIZER_FLAGS) $(FUZZ_TEST_SRCS) -o $@


.PHONY: fuzz

fuzz: $(BUILD_DIR)/fuzz_parser
	ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	./$(BUILD_DIR)/fuzz_parser


# ==========================================
# Clean
# ==========================================

.PHONY: clean

clean:
	rm -f $(BUILD_DIR)/server
	rm -f $(BUILD_DIR)/server_asan
	rm -f $(BUILD_DIR)/client
	rm -f $(BUILD_DIR)/client2
	rm -f $(BUILD_DIR)/test_parser
	rm -f $(BUILD_DIR)/client_load
	rm -f $(BUILD_DIR)/fuzz_parser
