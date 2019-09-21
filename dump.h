#ifndef dump_h_
#define dump_h_

#define DUMP_TEMPVAL_NAME dump_vp_
#define DUMP_RECURSIVE_LEVEL 2

#ifdef __cplusplus
extern "C" {
#endif

    int dump_open(const char* file_name, void* base_addr = nullptr);

    void dump(void* p, const char* type);

    void dump_s(void* p, const char* name, const char* file, int line);

#ifdef NDEBUG
# define p(v)
#else
/*# define p(v) dump_s(&v, __STRING(v), __FILE__, __LINE__) */
# define p(v)                                               \
    do {                                                    \
        typeof(v)* DUMP_TEMPVAL_NAME = &(v);                \
        dump_s(&DUMP_TEMPVAL_NAME, __STRING(v), __FILE__, __LINE__); \
    } while(0)
# define pv(v)                                              \
    do {                                                    \
        typeof(v) DUMP_TEMPVAL_NAME = (v);                  \
        dump_s(&DUMP_TEMPVAL_NAME, __STRING(v), __FILE__, __LINE__); \
    } while(0)
#endif

#ifdef __cplusplus
}
#endif

#endif // ! dump_h_
