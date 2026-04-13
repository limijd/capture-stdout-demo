CC = gcc
CFLAGS = -Wall -Wextra -g

all: demo

# 编译客户 .so
libcustomer.so: customer.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $<

# 编译主程序，链接客户 .so
demo: main.c capture.c libcustomer.so
	$(CC) $(CFLAGS) -o $@ main.c capture.c -L. -lcustomer -Wl,-rpath,.

run: demo
	./demo
	@echo ""
	@echo "--- log 文件内容 ---"
	@cat output.log

clean:
	rm -f demo libcustomer.so output.log

.PHONY: all run clean
