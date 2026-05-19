CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -g
CXXFLAGS = -Wall -Wextra -g -std=c++17
THREAD_FLAGS = -pthread

all: demo

libcustomer.so: customer.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $<

# capture.cpp 是 C++ 实现，但通过 extern "C" 暴露 C ABI 接口
capture.o: capture.cpp capture.hh
	$(CXX) $(CXXFLAGS) $(THREAD_FLAGS) -c -o $@ $<

# demo: main.c (C) 链接 capture.o (C++)。用 $(CXX) 作 driver 拉入 C++ runtime
demo: main.c capture.o libcustomer.so
	$(CXX) $(CFLAGS) $(THREAD_FLAGS) -o $@ \
	    -x c main.c \
	    -x none capture.o \
	    -L. -lcustomer -Wl,-rpath,.

run: demo
	./demo
	@echo ""
	@echo "--- log 文件内容 ---"
	@cat output.log

# ---- tests ----
# 用 g++ 作 driver；.c 文件用 -x c 强制按 C 编译，capture.o 与 .cpp 单元已编好
tests/test_iostream.o: tests/test_iostream.cpp tests/framework.h tests/helpers.h capture.hh
	$(CXX) $(CXXFLAGS) $(THREAD_FLAGS) -c -o $@ $<

test_capture: capture.o tests/test_capture.c tests/helpers.c tests/test_iostream.o
	$(CXX) $(CFLAGS) $(THREAD_FLAGS) -o $@ \
	    -x c tests/test_capture.c \
	    -x c tests/helpers.c \
	    -x none capture.o \
	    -x none tests/test_iostream.o

test: test_capture
	./test_capture

clean:
	rm -f demo libcustomer.so output.log test_capture capture.o tests/test_iostream.o
	rm -rf demo.dSYM libcustomer.so.dSYM test_capture.dSYM

.PHONY: all run test clean
