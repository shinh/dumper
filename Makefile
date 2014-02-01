CFLAGS = -g -Wall -W
LDFLAGS = -lelf -ldwarf
OBJS = test_dump.o dump.o
EXES = test_dump

all: $(EXES)

test_dump: $(OBJS)
	$(CXX) -o test_dump $(OBJS) $(LDFLAGS) $(CFLAGS)

.cc.o:
	$(CXX) -c $(CFLAGS) $<

clean:
	$(RM) -f $(OBJS) $(EXES) test_dump_misc

misc: test_dump_misc

test_dump_misc: all test_dump_misc.cc
	g++ -g -o $@ test_dump_misc.cc `sdl-config --cflags --libs` dump.o $(LDFLAGS)
