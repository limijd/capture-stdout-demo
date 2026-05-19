CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -g
CXXFLAGS = -Wall -Wextra -g -std=c++17
THREAD_FLAGS = -pthread

all: demo

libcustomer.so: customer.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $<

demo: main.c capture.c libcustomer.so
	$(CC) $(CFLAGS) $(THREAD_FLAGS) -o $@ main.c capture.c -L. -lcustomer -Wl,-rpath,.

run: demo
	./demo
	@echo ""
	@echo "--- log 文件内容 ---"
	@cat output.log

# ---- tests ----
# 分两步：先 g++ 编译 iostream .cpp 到 .o（带 -std=c++17），
# 然后 g++ 作为 driver 编 .c 文件并链接所有 .o；用 g++ 链接以拉入 C++ runtime。
tests/test_iostream.o: tests/test_iostream.cpp tests/framework.h tests/helpers.h capture.h
	$(CXX) $(CXXFLAGS) $(THREAD_FLAGS) -c -o $@ $<

test_capture: capture.c tests/test_capture.c tests/helpers.c tests/test_iostream.o
	$(CXX) $(CFLAGS) $(THREAD_FLAGS) -o $@ \
	    -x c tests/test_capture.c \
	    -x c tests/helpers.c \
	    -x c capture.c \
	    -x none tests/test_iostream.o

test: test_capture
	./test_capture

clean:
	rm -f demo libcustomer.so output.log test_capture tests/test_iostream.o
	rm -rf demo.dSYM libcustomer.so.dSYM test_capture.dSYM

.PHONY: all run test clean
