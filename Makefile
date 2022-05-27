.PHONY: run
run: main
	./main

main: main.c src/block.h src/block.c src/skinny.c src/skinny.h
	gcc -Werror -g -Isrc -o main main.c src/skinny.c src/block.c

.PHONY: clean
clean:
	rm -f main

fs.img:
	bash ./mkfs.sh