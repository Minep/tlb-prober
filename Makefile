
.PHONY: modtlbi
modtlbi:
	$(MAKE) -C modtlbi all


.PHONY: all clean
all:
	$(CC) evaluate.c -o evaluate

clean:
	@rm evaluate