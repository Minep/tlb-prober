
.PHONY: modtlbi
modtlbi:
	$(MAKE) -C modtlbi all


.PHONY: all clean
all:
	$(CC) evaluate.c -o evaluate

install:
	@sudo insmod modtlbi/modtlbi.ko

clean:
	@rm -f evaluate