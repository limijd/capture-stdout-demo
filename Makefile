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
test_capture: capture.c tests/test_capture.c tests/helpers.c
	$(CC) $(CFLAGS) $(THREAD_FLAGS) -o $@ \
	    tests/test_capture.c tests/helpers.c capture.c

test: test_capture
	./test_capture

clean:
	rm -f demo libcustomer.so output.log test_capture
	rm -rf demo.dSYM libcustomer.so.dSYM test_capture.dSYM

.PHONY: all run test clean
