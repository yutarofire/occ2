SRCS=$(wildcard *.c)
OBJS=$(SRCS:.c=.o)

TEST_SRCS=$(wildcard test/*.c)
TESTS=$(TEST_SRCS:.c=.exe)

occ: $(OBJS)
	$(CC) -o $@ $^

$(OBJS): occ.h

test/%.exe: occ test/%.c
	$(CC) -o- -E -P -C test/$*.c | ./occ - > test/$*.s
	$(CC) -xc -c -o test/common.o test/common
	$(CC) -o $@ test/$*.s test/common.o

test: $(TESTS)
	for i in $^; do echo $$i; ./$$i || exit 1; echo; done

clean:
	rm -f occ *.o tmp* test/*.o test/*.s test/*.exe

.PHONY: test clean
