CFLAGS=-O3 -Wall -Wextra -Wcast-qual -DDEBUG

all: deb juxt brotli-02-edit.txt
test: juxt
	./juxt -v testdata/*.compressed
deb: deb.o yeast.o try.o
juxt: juxt.o yeast.o try.o
deb.o: deb.c yeast.h
juxt.o: juxt.c yeast.h
yeast.o: yeast.c yeast.h xforms.h dict.h try.h
try.o: try.c try.h
brotli-02-edit.txt: brotli-02-edit.nroff
	./rfc-format.py $< > $@

clean:
	@rm -rf *.o deb juxt brotli-02-edit.txt
