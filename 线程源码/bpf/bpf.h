#ifndef __MINIMAL_USER_BPF_H
#define __MINIMAL_USER_BPF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BPF_ANY
#define BPF_ANY 0
#endif

int bpf_map_update_elem(int fd, const void* key, const void* value, uint64_t flags);
int bpf_map_lookup_elem(int fd, const void* key, void* value);
int bpf_map_delete_elem(int fd, const void* key);
int bpf_map_get_next_key(int fd, const void* key, void* next_key);

#ifdef __cplusplus
}
#endif

#endif
