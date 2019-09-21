#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>
#include <map>

#include "dump.h"

#ifdef __PIE__
extern char __executable_start[1];
#endif

typedef struct TestDump_ {
    short s;
    int i;
    long l;
    size_t st;
    unsigned long long ll;
    char c;
    const char* str;
    const char** strp;
    void* ptr;
    void* const volatile * cvptr;
    struct TestDump_* dump;
    int (*fp) (int, char*[]);
    int (*ifp) (int, char*[]);
    int array[10];
    enum TestEnum { ENUM1, ENUM2 };
    TestEnum en;
    union TestUnion {
        int i;
        char b[4];
    };
    TestUnion un;
    struct {
    } no_name_struct;
} TestDump;

class TestCppBase {
public:
    TestCppBase() : selfBase(*this) {
    }
    const TestCppBase& selfBase;
};

class TestCpp : public TestCppBase {
public:
    TestCpp() : self(*this) {
        cppstr = "fuga-";
        cppvec.push_back(1);
        cppmap[2] = 3;
    }
    std::string cppstr;
    std::vector<int> cppvec;
    std::map<int, int> cppmap;
    const TestCpp& self;
};

int main(int argc, char* argv[]) {
    TestDump d;
    d.s = 2;
    d.i = 3;
    d.l = 4;
    d.st = 5;
    d.ll = 0xfffffffffffll;
    d.c = 'c';
    d.str = "hoge-";
    d.strp = &d.str;
    d.ptr = &d;
    d.cvptr = &d.ptr;
    d.fp = main;
    d.en = TestDump::ENUM2;
    d.array[0] = 1;
    d.dump = &d;
    strcpy(d.un.b, "abc");

    void* base_addr = nullptr;
#ifdef __PIE__
    base_addr = __executable_start;
#endif

    dump_open(argv[0], base_addr);

    p(argc);
//    pv(argc);

    p(d);
//    pv(d);
//    dump(&d, "TestDump");

    TestCpp cpp;
    p(cpp);
//    pv(cpp);
//    dump(&cpp, "TestCpp");

/*
    void* vp;
    vp = &vp;
    p(vp);
*/

    return 0;
}
