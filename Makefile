CFLAGS=-O3 -Wall -Wextra -Wcast-qual -DDEBUG
CXXFLAGS=-O3 -Wall -Wextra -std=c++11
LDFLAGS=-lpthread

all: deb juxt brogen brotli-02-edit.txt
test: juxt
	./juxt -v testdata/*.compressed
deb: deb.o load.o yeast.o try.o
juxt: juxt.o load.o yeast.o try.o
deb.o: deb.c load.h yeast.h try.h
juxt.o: juxt.c load.h yeast.h try.h
load.o: load.c load.h
yeast.o: yeast.c yeast.h xforms.h dict.h try.h
try.o: try.c try.h
huff.c: huff.h
flatten.c: flatten.h
brogen: brogen.o huff.o flatten.o
	c++ -o $@ $^
brogen.o: brogen.cc huff.h flatten.h
brotli-02-edit.txt: brotli-02-edit.nroff
	./rfc-format.py $< > $@

clean:
	@rm -rf *.o deb juxt brogen
