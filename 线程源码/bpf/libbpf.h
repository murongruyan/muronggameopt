#ifndef __MINIMAL_LIBBPF_H
#define __MINIMAL_LIBBPF_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum libbpf_print_level {
    LIBBPF_WARN,
    LIBBPF_INFO,
    LIBBPF_DEBUG,
};

typedef int (*libbpf_print_fn_t)(enum libbpf_print_level level, const char* format, va_list args);

struct bpf_object;
struct bpf_map;
struct bpf_program;
struct bpf_link;
struct perf_buffer;

struct bpf_map_skeleton {
    const char* name;
    struct bpf_map** map;
    void** mmaped;
};

struct bpf_prog_skeleton {
    const char* name;
    struct bpf_program** prog;
    struct bpf_link** link;
};

struct bpf_object_skeleton {
    size_t sz;
    const char* name;
    const void* data;
    size_t data_sz;
    struct bpf_object** obj;
    int map_cnt;
    int map_skel_sz;
    struct bpf_map_skeleton* maps;
    int prog_cnt;
    int prog_skel_sz;
    struct bpf_prog_skeleton* progs;
};

struct bpf_object_open_opts {
    size_t sz;
    const char* object_name;
    const char* pin_root_path;
    const char* btf_custom_path;
    uint32_t kernel_log_level;
    size_t kernel_log_size;
    char* kernel_log_buf;
};

typedef void (*perf_buffer_sample_fn)(void* ctx, int cpu, void* data, uint32_t size);
typedef void (*perf_buffer_lost_fn)(void* ctx, int cpu, uint64_t lost_cnt);

struct perf_buffer_opts {
    size_t sz;
};

int libbpf_set_print(libbpf_print_fn_t fn);
long libbpf_get_error(const void* ptr);

int bpf_map__fd(const struct bpf_map* map);
const char* bpf_map__name(const struct bpf_map* map);
int bpf_map__set_autocreate(struct bpf_map* map, bool autocreate);

struct bpf_object* bpf_object__open_file(const char* path, const struct bpf_object_open_opts* opts);
struct bpf_object* bpf_object__open_mem(const void* obj_buf, size_t obj_buf_sz, const struct bpf_object_open_opts* opts);
int bpf_object__load(struct bpf_object* obj);
void bpf_object__close(struct bpf_object* obj);
struct bpf_map* bpf_object__find_map_by_name(const struct bpf_object* obj, const char* name);
struct bpf_map* bpf_object__next_map(const struct bpf_object* obj, const struct bpf_map* map);
struct bpf_program* bpf_object__find_program_by_name(const struct bpf_object* obj, const char* name);

int bpf_object__open_skeleton(struct bpf_object_skeleton* s, const struct bpf_object_open_opts* opts);
int bpf_object__load_skeleton(struct bpf_object_skeleton* s);
int bpf_object__attach_skeleton(struct bpf_object_skeleton* s);
void bpf_object__detach_skeleton(struct bpf_object_skeleton* s);
void bpf_object__destroy_skeleton(struct bpf_object_skeleton* s);

int bpf_program__set_autoload(struct bpf_program* prog, bool autoload);
struct bpf_link* bpf_program__attach(struct bpf_program* prog);
struct bpf_link* bpf_program__attach_raw_tracepoint(const struct bpf_program* prog, const char* tp_name);
void bpf_link__destroy(struct bpf_link* link);

struct perf_buffer* perf_buffer__new(int map_fd,
                                     size_t page_cnt,
                                     perf_buffer_sample_fn sample_cb,
                                     perf_buffer_lost_fn lost_cb,
                                     void* ctx,
                                     const struct perf_buffer_opts* opts);
int perf_buffer__poll(struct perf_buffer* pb, int timeout_ms);
void perf_buffer__free(struct perf_buffer* pb);

#ifdef __cplusplus
}
#endif

#endif
