
OBJS := evaluate.o

OBJS_JMP := jump_table_fit.o \
			jump_table_small.o \
			jump_table_scrap.o \
			evaluate.jmp.o

.PHONY: modtlbi
modtlbi:
	$(MAKE) -C modtlbi all

%.jmp.o: %.c
	$(CC) -DITLB_BENCH -c $< -o $@

%.o: %.c
	$(CC)  -c $< -o $@

%.o: %.S
	$(CC)  -c $< -o $@

%.ld: %.ldx
	$(CC) -E -x c -P $< -o $@

evaluate_jmp: $(OBJS_JMP)
	$(CC) -T link.ld -o $@ $^

evaluate: $(OBJS)
	$(CC) -o $@ $^

.PHONY: all clean generate

generate:
	@./generate.py

all: link.ld evaluate_jmp evaluate

install:
	@sudo insmod modtlbi/modtlbi.ko

clean:
	@rm -f evaluate evaluate_jmp jump_table_*.S jump_table_*.ldx link.ld $(OBJS)