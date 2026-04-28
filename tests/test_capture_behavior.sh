#!/bin/sh
set -eu

make clean >/dev/null
make >/dev/null

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT INT TERM

./demo >"$tmpdir/stdout.txt" 2>"$tmpdir/stderr.txt"

grep -F "我的程序: 准备调用客户代码" "$tmpdir/stdout.txt" >/dev/null
grep -F "客户 printf: hello from customer" "$tmpdir/stdout.txt" >/dev/null
grep -F "客户 fprintf: some data = 42" "$tmpdir/stdout.txt" >/dev/null
grep -F "客户 write: raw write to stdout" "$tmpdir/stdout.txt" >/dev/null
grep -F "客户 stderr: warning something" "$tmpdir/stderr.txt" >/dev/null

grep -F "我的程序: 准备调用客户代码" output.log >/dev/null
grep -F "客户 printf: hello from customer" output.log >/dev/null
grep -F "客户 fprintf: some data = 42" output.log >/dev/null
grep -F "客户 write: raw write to stdout" output.log >/dev/null
grep -F "客户 stderr: warning something" output.log >/dev/null
