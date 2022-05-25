.PHONY: run
run: main
	./main

main: main.c
	gcc -Isrc -o main main.c

.PHONY: clean
clean:
	rm -f main

fs.img:
	bash ./mkfs.sh