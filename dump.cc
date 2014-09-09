#include "dump.h"

#include <libdwarf.h>
#include <dwarf.h>
#include <libelf.h>
#define HAVE_ELF64_GETEHDR

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

#include <vector>
#include <set>
#include <map>
#include <string>
#include <sstream>
#include <algorithm>

using namespace std;

static Dwarf_Debug dbg;
static string processing_cu;
static map<string, class DumpUnit*> types;
static map<int, class DumpUnit*> id2unit;
struct func {
    string name;
    void* low;
    void* high;
};
static vector<func> funcs;
struct variable {
    string name;
    string file;
    int line;
    int type;
};
static map<string, vector<variable> > variables;
static set<void*> disp_ptrs;
static char** srcfiles;
static Dwarf_Signed srcnum;


static void print_error(const char* msg, int dwarf_code, Dwarf_Error err) {
    if (dwarf_code == DW_DLV_ERROR) {
        const char* errmsg = dwarf_errmsg(err);
        long long myerr = dwarf_errno(err);

        fprintf(stderr, "ERROR:  %s:  %s (%lld)\n", msg, errmsg, myerr);
    } else if (dwarf_code == DW_DLV_NO_ENTRY) {
        fprintf(stderr, "NO ENTRY:  %s: \n", msg);
    } else if (dwarf_code == DW_DLV_OK) {
        fprintf(stderr, "%s \n", msg);
    } else {
        fprintf(stderr, "InternalError:  %s:  code %d\n", msg, dwarf_code);
    }
}

static bool is_readable(void *ptr) {
    int fd[2];
    if (pipe(fd)) {
        perror("pipe(2) failed");
        exit(1);
    }
    bool readable = (write(fd[1], ptr, 1) == 1);
    close(fd[0]);
    close(fd[1]);
    return readable;
}

struct DwarfException {};

namespace {
    static void print_escaped(const char* s, int n) {
        for (int i = 0; i < n; i++, s++) {
            if (isprint(*s)) printf("%c", *s);
            else printf("\\x%02x", (unsigned char)*s);
        }
    }

    static void dump_str(char* str, int size = -1) {
        if (size == -1) size = strlen(str);
        if (size < 50) {
            printf("\"");
            print_escaped(str, size);
            printf("\" [%p]", str);
        }
        else {
            static const int BUFSIZE = 50;
            char buf[BUFSIZE];
            strncpy(buf, str, BUFSIZE);
            buf[BUFSIZE-1] = '\0';
            printf("\"");
            print_escaped(buf, BUFSIZE);
            printf("...\" [%p]", str);
        }
    }

    static Dwarf_Half getTag(Dwarf_Die die) {
        int ret;
        Dwarf_Error err;
        Dwarf_Half tag;
        ret = dwarf_tag(die, &tag, &err);
        if (ret != DW_DLV_OK) {
            print_error("dwarf_tag", ret, err);
            throw DwarfException();
        }
        return tag;
    }

    static int getAttr(Dwarf_Die die, Dwarf_Half an, const char* ans,
                       Dwarf_Attribute* attr)
    {
        int ret;
        Dwarf_Error err;
        ret = dwarf_attr(die, an, attr, &err);
        if (ret == DW_DLV_NO_ENTRY) {
            return 1;
        }
        if (ret != DW_DLV_OK) {
            char buf[1024];
            sprintf(buf, "dwarf_attr %s", ans);
            print_error(buf, ret, err);
            throw DwarfException();
        }
        return 0;
    }
    static int getAttrInt(Dwarf_Die die, Dwarf_Half an, const char* ans) {
        Dwarf_Attribute attr;
        Dwarf_Error err;
        int ret;
        Dwarf_Unsigned size;

        if (getAttr(die, an, ans, &attr)) return -1;
        ret = dwarf_formudata(attr, &size, &err);
        if (ret != DW_DLV_OK) {
            print_error("dwarf_formudata", ret, err);
            throw DwarfException();
        }

        return size;
    }

    static int getSize(Dwarf_Die die) {
        return getAttrInt(die, DW_AT_byte_size, "byte_size");
    }
 
    static int getUpperBound(Dwarf_Die die) {
        return getAttrInt(die, DW_AT_upper_bound, "upper_bound");
    }
 
    static Dwarf_Addr getHighPc(Dwarf_Die die) {
        Dwarf_Addr pc;
        Dwarf_Error err;
        int ret;
        ret = dwarf_highpc(die, &pc, &err);
        if (ret == DW_DLV_NO_ENTRY) return 0;
        if (ret != DW_DLV_OK) {
            print_error("dwarf_highpc", ret, err);
            return 0;  // TODO

            throw DwarfException();
        }
        return pc;
    }
 
    static Dwarf_Addr getLowPc(Dwarf_Die die) {
        Dwarf_Addr pc;
        Dwarf_Error err;
        int ret;
        ret = dwarf_lowpc(die, &pc, &err);
        if (ret == DW_DLV_NO_ENTRY) return 0;
        if (ret != DW_DLV_OK) {
            print_error("dwarf_lowpc", ret, err);
            throw DwarfException();
        }
        return pc;
    }
 
    static string getName(Dwarf_Die die) {
        int ret;
        Dwarf_Error err;
        Dwarf_Attribute attr;
        char* str;

        ret = dwarf_attr(die, DW_AT_name, &attr, &err);
        if (ret == DW_DLV_NO_ENTRY) {
            return "<no name>";
        }
        if (ret != DW_DLV_OK) {
            print_error("dwarf_attr DW_AT_name", ret, err);
            throw DwarfException();
        }
        ret = dwarf_formstring(attr, &str, &err);
        if (ret != DW_DLV_OK) {
            print_error("dwarf_formstring DW_AT_name", ret, err);
            throw DwarfException();
        }

        return string(str);
    }

    static int getType(Dwarf_Die die,
                       Dwarf_Half an = DW_AT_type, string ans = "type")
    {
        int ret;
        Dwarf_Error err;
        Dwarf_Attribute attr;
        Dwarf_Off id, aoff, off;

        ret = dwarf_dieoffset(die, &aoff, &err);
        if (ret != DW_DLV_OK) {
            print_error("dwarf_dieoffset", ret, err);
            throw DwarfException();
        }
        ret = dwarf_die_CU_offset(die, &off, &err);
        if (ret != DW_DLV_OK) {
            print_error("dwarf_die_CU_offset", ret, err);
            throw DwarfException();
        }

        ret = dwarf_attr(die, an, &attr, &err);
        if (ret == DW_DLV_NO_ENTRY) {
            return 0;
        }
        else if (ret != DW_DLV_OK) {
            print_error(("dwarf_attr " + ans).c_str(), ret, err);
            throw DwarfException();
        }
        ret = dwarf_formref(attr, &id, &err);
        if (ret != DW_DLV_OK) {
            print_error("dwarf_formref", ret, err);
            throw DwarfException();
        }

        return aoff-off+id;
    }

    static int getLoc(Dwarf_Die die) {
        int ret;
        Dwarf_Error err;
        Dwarf_Attribute attr;
        Dwarf_Locdesc** loc;
        Dwarf_Signed size;

        ret = dwarf_attr(die, DW_AT_data_member_location, &attr, &err);
        if (ret == DW_DLV_NO_ENTRY) {
            return -1;
        }
        if (ret != DW_DLV_OK) {
            print_error("dwarf_attr DW_AT_member_location", ret, err);
            throw DwarfException();
        }
        ret = dwarf_loclist_n(attr, &loc, &size, &err);
        if (ret != DW_DLV_OK) {
            // Fall back?
            Dwarf_Unsigned l;
            int r = dwarf_formudata(attr, &l, &err);
            if (r == DW_DLV_OK) {
                return l;
            }

            print_error("dwarf_loclist_n", ret, err);
            throw DwarfException();
        }

        return loc[0]->ld_s[0].lr_number;
    }

}

class DumpUnit {
public:
    virtual void dump(void* p) =0;
    virtual string name() =0;
    virtual ~DumpUnit() {}
};

class DumpPrim : public DumpUnit {
public:
    DumpPrim(Dwarf_Die die) {
        name_ = getName(die);
        size_ = getSize(die);
        types[name_] = this;
    }

    virtual void dump(void* p) {
        if (size_ == 1) {
            if (name_.find("bool") == string::npos) {
                unsigned char c = *(char*)p;
                if (isprint(c)) printf("'%c' (%02x)", c, c);
                else printf("'\\x%02x' (%02x)", c, c);
            }
            else {
                bool* bp = (bool*)p;
                printf(*bp ? "true" : "false\n");
            }
        }
        else if (size_ == 2) {
            short* ip = (short*)p;
            printf("%d (0x%04x)", *ip, *ip);
        }
        else if (size_ == 4) {
            int* ip = (int*)p;
            printf("%d (0x%08x)", *ip, *ip);
        }
        else if (size_ == 8) {
            long long* ip = (long long*)p;
            printf("%lld (0x%016llx)", *ip, *ip);
        }
        else {
            printf("unimplemented primitive '%s'\n", name_.c_str());
            return;
        }
//        printf(" : %s\n", name_.c_str());
    }

    virtual string name() { return name_; }

private:
    string name_;
    int size_;
};

class DumpStruct : public DumpUnit {
public:
    DumpStruct(Dwarf_Die die, Dwarf_Half tag) {
        int ret;
        Dwarf_Error err;
        Dwarf_Die child;

        tag_ = tag;

        name_ = getName(die);
        if (name_ == "<no name>") {
            int id = getType(die, DW_AT_specification, "specification");
            DumpUnit* u = id2unit[id];
            if (u) name_ = u->name();
            else return;  // Ignore unnamed type.
        }
        types[name_] = this;

        ret = dwarf_child(die, &child, &err);
        if (ret == DW_DLV_NO_ENTRY) return;
        if (ret != DW_DLV_OK) {
            print_error("dwarf_child", ret, err);
            throw DwarfException();
        }
        addMember(child);

        while (1) {
            ret = dwarf_siblingof(dbg, child, &child, &err);
            if (ret == DW_DLV_NO_ENTRY) {
                break;
            }
            else if (ret != DW_DLV_OK) {
                print_error("dwarf_siblingof", ret, err);
                throw DwarfException();
            }
            addMember(child);
        }
    }

    virtual void dump(void* p) {
        disp_ptrs.insert(p);

        static int nest_level = 0;

        if (nest_level > DUMP_RECURSIVE_LEVEL*2) {
            printf("{ ... }");
            return;
        }

        printf("{\n");
        nest_level += 2;
        for (vector<Member>::iterator ite = members_.begin();
             ite != members_.end(); ++ite)
        {
            Member* mem = &*ite;
            char* mp = (char*)p;
            mp += mem->loc;
            for (int i = 0; i < nest_level; i++) printf(" ");
            printf("%s = ", mem->name.c_str());
            DumpUnit* u = id2unit[mem->type];
            if (u) {
                u->dump(mp);
                printf(" : %s\n", u->name().c_str());
            }
            else {
                printf("???\n");
            }
        }
        nest_level -= 2;
        for (int i = 0; i < nest_level; i++) printf(" ");
        printf("}");
    }

    virtual string name() {
        return name_;
    }

private:
    Dwarf_Half tag_;
    string name_;
    struct Member {
        string name;
        int type;
        int loc;
    };
    vector<Member> members_;

    void addMember(Dwarf_Die die) {
        Dwarf_Half tag = getTag(die);
        if (tag == DW_TAG_member) {
            Member mem;
            mem.name = getName(die);
            mem.type = getType(die);
            if (tag_ == DW_TAG_structure_type) mem.loc = getLoc(die);
            else mem.loc = 0;
            members_.push_back(mem);
        }
        else if (tag == DW_TAG_inheritance) {
            Member mem;
            mem.name = "<inherit>";
            mem.type = getType(die);
            mem.loc = getLoc(die);
            members_.push_back(mem);
        }
    }

};

class DumpTypedef : public DumpUnit {
public:
    DumpTypedef(Dwarf_Die die) {
        type_ = getType(die);
        name_ = getName(die);
        types[name_] = this;
    }

    virtual void dump(void* p) {
        DumpUnit* u = id2unit[type_];
        if (u) u->dump(p);
        else printf("<void>");
    }

    virtual string name() {
        return name_;
    }

private:
    string name_;
    int type_;
};

class DumpFunc : public DumpUnit {
public:
    DumpFunc(Dwarf_Die die) {
        int ret;
        Dwarf_Error err;
        Dwarf_Die child;

        ret = dwarf_child(die, &child, &err);
        if (ret == DW_DLV_NO_ENTRY) return;
        if (ret != DW_DLV_OK) {
            print_error("dwarf_child", ret, err);
            throw DwarfException();
        }
        args_.push_back(getType(child));

        while (1) {
            ret = dwarf_siblingof(dbg, child, &child, &err);
            if (ret == DW_DLV_NO_ENTRY) {
                break;
            }
            else if (ret != DW_DLV_OK) {
                print_error("dwarf_siblingof", ret, err);
                throw DwarfException();
            }
            args_.push_back(getType(child));
        }
        type_ = getType(die);
    }

    virtual void dump(void* p) {
        string type = "???";
        string args = "";
        DumpUnit* u = id2unit[type_];
        if (u) type = u->name();
        for (size_t i = 0; i < args_.size(); i++) {
            u = id2unit[args_[i]];
            if (i != 0) args += ", ";
            if (u) args += u->name();
            else args += "???";
        }

        void** vp = (void**)p;
        for (vector<func>::const_iterator ite = funcs.begin();
             ite != funcs.end(); ++ite)
        {
            if (ite->low == *vp) {
                printf("%s %s(%s)",
                       type.c_str(), ite->name.c_str(), args.c_str());
                return;
            }
        }
        printf("%s %s(%s)",
               type.c_str(), "???", args.c_str());
    }

    virtual string name() {
        return "func";
    }

private:
    int type_;
    vector<int> args_;

};

class DumpEnum : public DumpUnit {
public:
    DumpEnum(Dwarf_Die die) { 
        int ret;
        Dwarf_Error err;
        Dwarf_Die child;

        name_ = getName(die);

        ret = dwarf_child(die, &child, &err);
        if (ret == DW_DLV_NO_ENTRY) return;
        if (ret != DW_DLV_OK) {
            print_error("dwarf_child", ret, err);
            throw DwarfException();
        }
        add(child);

        while (1) {
            ret = dwarf_siblingof(dbg, child, &child, &err);
            if (ret == DW_DLV_NO_ENTRY) {
                break;
            }
            else if (ret != DW_DLV_OK) {
                print_error("dwarf_siblingof", ret, err);
                throw DwarfException();
            }
            add(child);
        }
    }

    virtual void dump(void* p) {
        int* ip = (int*)p;
        printf("%s", enums_[*ip].c_str());
    }

    virtual string name() {
        return name_;
    }

private:
    void add(Dwarf_Die die) {
        Dwarf_Attribute attr;
        Dwarf_Error err;
        int ret;
        Dwarf_Signed sval;
        Dwarf_Unsigned uval;
        int val;

        if (getAttr(die, DW_AT_const_value, "const_value", &attr)) {
            print_error("enum's child is not const_value", 0, DW_DLV_OK);
            throw DwarfException();
        }
        ret = dwarf_formudata(attr, &uval, &err);
        if (ret == DW_DLV_OK) {
            val = uval;
        }
        else {
            ret = dwarf_formsdata(attr, &sval, &err);
            if (ret != DW_DLV_OK) {
                print_error("dwarf_formsdata", ret, err);
                throw DwarfException();
            }
            val = sval;
        }

        enums_[val] = getName(die);
    }

    string name_;
    map<int, string> enums_;

};

class DumpCv : public DumpUnit {
public:
    DumpCv(Dwarf_Die die, Dwarf_Half tag) {
        tag_ = tag;
        type_ = getType(die);
    }

    virtual void dump(void* p) {
        DumpUnit* u = id2unit[type_];
        u->dump(p);
    }

    virtual string name() {
        DumpUnit* u = id2unit[type_];
        return u->name();
    }

private:
    Dwarf_Half tag_;
    int type_;
};

class DumpPtr : public DumpUnit {
public:
    DumpPtr(Dwarf_Die die, Dwarf_Half tag) {
        tag_ = tag;
        type_ = getType(die);
    }

    virtual void dump(void* p) {
        void** vp = (void**)p;

        if (!is_readable(p)) {
            printf("%p <invalid ptr>", *vp);
            return;
        }

        DumpUnit* u = id2unit[type_];
        if (!u) {
            printf("%p", *vp);
            return;
        }

        if (dynamic_cast<DumpStruct*>(u)) {
            if (disp_ptrs.find(*vp) != disp_ptrs.end()) {
                printf("%p <previously shown>", *vp);
                return;
            }
        }

        if (dynamic_cast<DumpPrim*>(u) && u->name() == "char") {
            dump_str(*(char**)p);
        }
        else if (dynamic_cast<DumpFunc*>(u)) {
            u->dump(vp);
            printf(" [%p]", *vp);
        }
        else {
            u->dump(*vp);
            printf(" [%p]", *vp);
        }
    }

    virtual string name() {
        string p = (tag_ == DW_TAG_pointer_type) ? "*" : "&";
        if (type_ == 0) return "void" + p;
        DumpUnit* u = id2unit[type_];
        if (!u) return "???" + p;
        return u->name() + p;
    }

private:
    Dwarf_Half tag_;
    int type_;
};

class DumpArray : public DumpUnit {
public:
    DumpArray(Dwarf_Die die) {
        int ret;
        Dwarf_Error err;
        Dwarf_Die child;
        type_ = getType(die);
        ret = dwarf_child(die, &child, &err);
        if (ret != DW_DLV_OK) {
            print_error("dwarf_child", ret, err);
            throw DwarfException();
        }
        if (getTag(child) != DW_TAG_subrange_type) {
            print_error("array's child is not subrange", DW_DLV_OK, err);
            throw DwarfException();
        }
        size_ = getUpperBound(child) + 1;
    }

    virtual void dump(void* p) {
        if (size_ < 1) {
            printf("{}");
            return;
        }
        DumpUnit* u = id2unit[type_];
        if (dynamic_cast<DumpPrim*>(u) && u->name() == "char") {
            dump_str((char*)p, size_);
/*
            char* str = (char*)p;
            if (size_ < 50 && strlen(str) < 50) {
                printf("\"%s\"", str);
            }
            else {
                static const int BUFSIZE = 50;
                char buf[BUFSIZE];
                strncpy(buf, str, BUFSIZE);
                buf[BUFSIZE-1] = '\0';
                printf("\"%s...\"", buf);
            }
*/
        }
        else if (u) {
            printf("{ ");
            u->dump(p);
            if (size_ > 1) printf(", ...");
            printf(" }");
        }
        else {
            printf("{ ???, ... }");
        }
    }

    virtual string name() {
        ostringstream oss;
        DumpUnit* u = id2unit[type_];
        if (u) oss << u->name();
        else oss << "???";
        oss << "[" << size_ << "]";
        return oss.str();
    }

private:
    int type_;
    int size_;
};

static void add_func(Dwarf_Die die) {
    func f;
    f.name = getName(die);
    f.low = (void*)getLowPc(die);
    f.high = (void*)getHighPc(die);
    if (f.low && f.high) funcs.push_back(f);
}

static void add_line(Dwarf_Die die) {
    variable v;
    int f = getAttrInt(die, DW_AT_decl_file, "decl_file");
    v.line = getAttrInt(die, DW_AT_decl_line, "decl_line");
    if (v.line == -1 || f == -1) return;
    if (srcfiles && f > 0 && f <= srcnum) {
        v.file = srcfiles[f-1];
    }
    v.name = getName(die);
    v.type = getType(die);
//    printf("%s:%d %s\n", v.file.c_str(), v.line, v.name.c_str());
    variables[processing_cu].push_back(v);
}

static int open_info(Dwarf_Die die, int d) {
    Dwarf_Error err;
    int ret;

    while (1) {
        Dwarf_Half tag;
        Dwarf_Off aoff, off;
        DumpUnit* unit = 0;

        tag = getTag(die);

        try {
            if (tag == DW_TAG_base_type) {
                unit = new DumpPrim(die);
            }
            else if (tag == DW_TAG_structure_type ||
                     tag == DW_TAG_union_type)
            {
                unit = new DumpStruct(die, tag);
            }
            else if (tag == DW_TAG_reference_type ||
                     tag == DW_TAG_pointer_type)
            {
                unit = new DumpPtr(die, tag);
            }
            else if (tag == DW_TAG_const_type ||
                     tag == DW_TAG_volatile_type)
            {
                unit = new DumpCv(die, tag);
            }
            else if (tag == DW_TAG_typedef) {
                unit = new DumpTypedef(die);
            }
            else if (tag == DW_TAG_subroutine_type) {
                unit = new DumpFunc(die);
            }
            else if (tag == DW_TAG_array_type) {
                unit = new DumpArray(die);
            }
            else if (tag == DW_TAG_enumeration_type) {
                unit = new DumpEnum(die);
            }
            else {
                if (tag == DW_TAG_subprogram) {
                    add_func(die);
                }
                else if (tag == DW_TAG_variable ||
                         tag == DW_TAG_formal_parameter)
                {
                    add_line(die);
                }
                else if (tag == DW_TAG_compile_unit) {
                    processing_cu = getName(die);
                }
                goto next;
            }
        }
        catch (...) {
            return 1;
        }

        ret = dwarf_dieoffset(die, &aoff, &err);
        if (ret != DW_DLV_OK) {
            print_error("dwarf_dieoffset", ret, err);
            return ret;
        }
        ret = dwarf_die_CU_offset(die, &off, &err);
        if (ret != DW_DLV_OK) {
            print_error("dwarf_die_CU_offset", ret, err);
            return ret;
        }

        id2unit[aoff] = unit;
/*
        for (int i = 0; i < d; i++) putc(' ', stdout);
//        printf("<%d>%d: %s\n", aoff, tag, str);
        printf("%d ", aoff);
        printf("%d: %s\n", tag, str);
*/
    next:
        Dwarf_Die child;
        ret = dwarf_child(die, &child, &err);
        if (ret == DW_DLV_OK) {
            open_info(child, d+1);
        }
        else if (ret == DW_DLV_ERROR) {
            print_error("dwarf_child", ret, err);
            return 1;
        }

        ret = dwarf_siblingof(dbg, die, &die, &err);
        if (ret == DW_DLV_NO_ENTRY) {
            break;
        }
        else if (ret != DW_DLV_OK) {
            print_error("dwarf_siblingof", ret, err);
            return ret;
        }
    }

    return 0;
}

static int open_infos() {
    Dwarf_Die die = 0;
    Dwarf_Error err;
    int ret;

    Dwarf_Unsigned cu_header_length = 0;
    Dwarf_Unsigned abbrev_offset = 0;
    Dwarf_Half version_stamp = 0;
    Dwarf_Half address_size = 0;
    Dwarf_Unsigned next_cu_offset = 0;

    while ((ret =
            dwarf_next_cu_header(dbg, &cu_header_length, &version_stamp,
                                 &abbrev_offset, &address_size,
                                 &next_cu_offset, &err))
           == DW_DLV_OK)
    {
        ret = dwarf_siblingof(dbg, NULL, &die, &err);

        ret = dwarf_srcfiles(die, &srcfiles, &srcnum, &err);
        if (ret != DW_DLV_OK) {
            srcfiles = 0;
            srcnum = 0;
        }

        if (ret == DW_DLV_OK) {
            ret = open_info(die, 0);
            if (ret) return ret;
        }
        else if (ret == DW_DLV_NO_ENTRY) {
            continue;
        }
        else if (ret != DW_DLV_OK) {
            print_error("dwarf_siblingof", ret, err);
            return ret;
        }

        if (srcfiles) {
            for (int i = 0; i < srcnum; i++) {
                dwarf_dealloc(dbg, srcfiles[i], DW_DLA_STRING);
            }
            dwarf_dealloc(dbg, srcfiles, DW_DLA_LIST);
        }
    }

    if (ret == DW_DLV_ERROR) {
        print_error("dwarf_next_cu_header", ret, err);
        return ret;
    }

    return 0;
}

static int process_one_file(Elf* elf, const char* file_name, int archive) {
    int dres;
    Dwarf_Error err;
    int ret;

    dres = dwarf_elf_init(elf, DW_DLC_READ, NULL, NULL, &dbg, &err);
    if (dres == DW_DLV_NO_ENTRY) {
        printf("No DWARF information present in %s\n", file_name);
        return 0;
    }
    if (dres != DW_DLV_OK) {
        print_error("dwarf_elf_init", dres, err);
        return 1;
    }

    if (archive) {
        Elf_Arhdr *mem_header = elf_getarhdr(elf);

        printf("\narchive member \t%s\n",
               mem_header ? mem_header->ar_name : "");
    }

//    print_infos();
    ret = open_infos();

    return ret;
}

extern "C" int dump_open(const char* file_name) {
    int f;
    Elf_Cmd cmd;
    Elf *arf, *elf;
    int archive = 0;
    int ret = 0;

    elf_version(EV_NONE);
    if (elf_version(EV_CURRENT) == EV_NONE) {
        fprintf(stderr, "dwarfdump: libelf.a out of date.\n");
        return 1;
    }

#ifdef __CYGWIN__
    f = open(file_name, O_RDONLY | O_BINARY);
#else
    f = open(file_name, O_RDONLY);
#endif
    if (f == -1) {
        fprintf(stderr, "ERROR:  can't open %s\n", file_name);
        return 1;
    }

    cmd = ELF_C_READ;
    arf = elf_begin(f, cmd, (Elf *) 0);
    if (elf_kind(arf) == ELF_K_AR) {
        archive = 1;
    }
    while ((elf = elf_begin(f, cmd, arf)) != 0) {
        Elf32_Ehdr *eh32;

#ifdef HAVE_ELF64_GETEHDR
        Elf64_Ehdr *eh64;
#endif /* HAVE_ELF64_GETEHDR */
        eh32 = elf32_getehdr(elf);
        if (!eh32) {
#ifdef HAVE_ELF64_GETEHDR
            /* not a 32-bit obj */
            eh64 = elf64_getehdr(elf);
            if (!eh64) {
                /* not a 64-bit obj either! */
                /* dwarfdump is quiet when not an object */
            } else {
                ret |= process_one_file(elf, file_name, archive);
            }
#endif /* HAVE_ELF64_GETEHDR */
        } else {
            ret |= process_one_file(elf, file_name, archive);
        }
        cmd = elf_next(elf);
        elf_end(elf);
    }
    elf_end(arf);
    return ret;
}

extern "C" void dump(void* p, const char* type) {
    disp_ptrs.clear();
//    disp_ptrs.insert(p);

    string name(type);
    map<string, DumpUnit*>::iterator ite = types.find(name);
    if (ite != types.end()) {
        ite->second->dump(p);
    }
    printf("\n");
}

extern "C" void dump_s(void* p, const char* name, const char* file, int line) {
    disp_ptrs.clear();
//    disp_ptrs.insert(p);

    map<string, vector<variable> >::iterator vals = variables.find(file);
    if (vals == variables.end()) {
        printf("cannot find debug_info of %s\n", file);
        return;
    }

    int type = -1;
    for (vector<variable>::iterator ite = vals->second.begin();
         ite != vals->second.end(); ++ite)
    {
        if (ite->file.find(file) != string::npos && ite->line > line) continue;
        if (ite->name == "dump_vp_") {
            type = ite->type;
        }
    }

    if (type == -1) {
        printf("cannot find type of %s\n", name);
        return;
    }

    DumpUnit* u = id2unit[type];
    if (!u) {
        printf("cannot find type info of %s\n", name);
        return;
    }

    printf("%s = ", name);
    u->dump(p);
    printf(" : %s\n", u->name().c_str());
}
