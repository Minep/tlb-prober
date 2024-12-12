
OBJS := jump_table_fit.o \
		jump_table_small.o \
		jump_table_scrap.o \
		evaluate.o

.PHONY: modtlbi
modtlbi:
	$(MAKE) -C modtlbi all

%.o: %.c
	$(CC) -c $< -o $@

%.o: %.S
	$(CC) -c $< -o $@

%.ld: %.ldx
	$(CC) -E -x c -P $< -o $@

evaluate_jmp: $(OBJS)
	$(CC) -T link.ld -o $@ $^

evaluate: evaluate.o
	$(CC) -o $@ $^

.PHONY: all clean generate

generate:
	@./generate.py

all: generate link.ld evaluate_jmp evaluate

install:
	@sudo insmod modtlbi/modtlbi.ko

clean:
	@rm -f evaluate evaluate_jmp jump_table_*.S jump_table_*.ldx link.ld $(OBJS)