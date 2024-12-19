
OBJS := evaluate.o

OBJS_JMP := evaluate.jmp.o

CFLAGS := -g -Og

%.jmp.o: %.c
	$(CC) $(CFLAGS) -DITLB_BENCH -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC)  -c $< -o $@

%.ld: %.ldx
	$(CC) -E -x c -P $< -o $@

evaluate_jmp: $(OBJS_JMP)
	$(CC) $(CFLAGS) -o $@ $^

evaluate: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

.PHONY: all clean generate

generate:
	@./generate.py

all: evaluate_jmp evaluate

install:
	@sudo insmod modtlbi/modtlbi.ko

clean:
	@rm -f evaluate evaluate_jmp jump_table_*.S jump_table_*.ldx $(OBJS) $(OBJS_JMP)


run_dtlb:
	@./run.sh evaluate

run_itlb:
	@./run.sh evaluate_jmp