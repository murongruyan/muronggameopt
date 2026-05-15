#ifndef __MINIMAL_BPF_CORE_READ_H
#define __MINIMAL_BPF_CORE_READ_H

#define __CORE_RELO(src) __builtin_preserve_access_index(src)

#define BPF_CORE_READ_INTO(dst, src, field)                                           \
    ({                                                                                \
        const void *__p = (const void *)__CORE_RELO(&(src)->field);                   \
        bpf_probe_read_kernel((dst), sizeof(*(dst)), __p);                            \
    })

#define BPF_CORE_READ(src, field)                                                     \
    ({                                                                                \
        typeof((src)->field) __val = 0;                                               \
        BPF_CORE_READ_INTO(&__val, src, field);                                       \
        __val;                                                                        \
    })

#define bpf_core_read_str(dst, sz, src) bpf_probe_read_kernel((dst), (sz), __CORE_RELO(src))

#endif
