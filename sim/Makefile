.PHONY: all
all: ledsim

ledsim: ledsim.c
	$(CC) -Wall -O2 ledsim.c -o ledsim

.PHONY: clean
clean:
	rm -f ledsim

.PHONY: run
run: ledsim
	sudo bash -c \
	  './ledsim led8500:{amber:{0..3},red:{1..3},blue:{1..3},green:{0..3}}'
