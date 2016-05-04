CFLAGS=-O3 -std=c99 -Wall -Wextra -Wcast-qual -Wno-deprecated-declarations -DDEBUG
CXXFLAGS=-O3 -Wall -Wextra -std=c++11
LDLIBS=-lpthread -lcrypto
# -lcrypto is for openssl functions on Mac OS X -- other systems use -lssl

all: deb juxt brogen brand broad braid brotli-02-edit.txt
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
xxhash.c: xxhash.h
crc32c.c: crc32c.h
brand.o: brand.c load.h yeast.h xxhash.h crc32c.h
brand: brand.o load.o yeast.o try.o xxhash.o crc32c.o
broad.o: broad.c load.h yeast.h xxhash.h crc32c.h
broad: broad.o load.o yeast.o try.o xxhash.o crc32c.o
braid.o: braid.c try.h
braid: braid.o try.o xxhash.o
brotli-02-edit.txt: brotli-02-edit.nroff
	./rfc-format.py $< > $@

clean:
	@rm -rf *.o deb juxt brogen brand broad braid
