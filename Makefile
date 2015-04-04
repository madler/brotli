CFLAGS=-O3 -Wall -Wextra -Wcast-qual -DDEBUG

all: deb juxt
test: juxt
	./juxt -v testdata/*.compressed
deb: deb.o yeast.o try.o
juxt: juxt.o yeast.o try.o
deb.o: deb.c yeast.h
juxt.o: juxt.c yeast.h
yeast.o: yeast.c yeast.h xforms.h dict.h try.h
try.o: try.c try.h

clean:
	@rm -rf *.o deb juxt
