all: pawprint

pawprint: pawprint.o
	$(CC) -o pawprint pawprint.o $(CFLAGS) $(LDFLAGS)

.PHONY: clean
clean:
	rm -f pawprint pawprint.o
