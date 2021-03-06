//
// Created by beich on 2020/11/8.
//

#include "linker_util.h"

#include <algorithm>
#include <sys/mman.h>
#include <cinttypes>

#include <maps_util.h>
#include <macros.h>


#define MAYBE_MAP_FLAG(x, from, to)  (((x) & (from)) ? (to) : 0)
#define PFLAGS_TO_PROT(x)            (MAYBE_MAP_FLAG((x), PF_X, PROT_EXEC) | \
                                      MAYBE_MAP_FLAG((x), PF_R, PROT_READ) | \
                                      MAYBE_MAP_FLAG((x), PF_W, PROT_WRITE))

#define MAX_OUT 8192

#define STRCAT_MES(msg) \
    do{                 \
        if (msg != nullptr) \
            strncat(out, msg, MAX_OUT); \
        else            \
            strncat(out, "null", MAX_OUT);\
    }while (0)

#define STRCAT_BOOLEAN(msg, cond) STRCAT_MES(msg); strncat(out, cond ? "true" : "false", MAX_OUT)
#define STRCAT_STRING(msg, str) STRCAT_MES(msg); STRCAT_MES(str);
#define STRCAT_HEX(msg, value) \
    do {                       \
        STRCAT_MES(msg);       \
        size_t len_ = strlen(out);\
        snprintf(&out[len_], (size_t)(MAX_OUT - len_), "0x%" SCNx64, (uint64_t)value);\
    } while (0)


std::string soinfo_to_string(soinfo *si) {
    if (si == nullptr) {
        return "soinfo is null.";
    }
    char out[MAX_OUT];
    out[0] = '\0';

    STRCAT_STRING("soinfo name: ", si->get_soname());
    STRCAT_HEX(", base address: ", si->base);
    STRCAT_HEX(", flags: ", si->flags_);
    STRCAT_HEX(", load bias: ", si->load_bias);

    auto format = [&](soinfo_list_t &list, const char *name) {
        STRCAT_STRING(name, "{");
        size_t size = list.size();
        list.for_each([&](soinfo *so) {
            STRCAT_MES(so->get_soname());
            if (--size != 0) {
                STRCAT_MES(", ");
            }
        });
        STRCAT_MES("}");
    };
    STRCAT_MES(", ");
    format(si->children_, "children");
    STRCAT_MES(", ");
    format(si->parents_, "parents");

#if __ANDROID_API__ >= __ANDROID_API_L_MR1__
    STRCAT_HEX(", rtld_flags: ", si->rtld_flags_);
#endif
#if __ANDROID_API__ >= __ANDROID_API_M__        // 6.0以上
    STRCAT_HEX(", dt_flags_1: ", si->dt_flags_1_);
    STRCAT_STRING(", realpath: ", si->realpath_.c_str());
#endif
#if __ANDROID_API__ >= __ANDROID_API_N__
    size_t len = si->dt_runpath_.size();
    STRCAT_MES(", dt_runpath:{");
    for (int i = 0; i < len; ++i) {
        STRCAT_MES(si->dt_runpath_[i].c_str());
        if (i != len - 1) {
            STRCAT_MES(", ");
        }
    }
    STRCAT_STRING("}, primary namespace: " ,si->primary_namespace_->get_name());
    STRCAT_MES(", secondary_namespaces{");
    len = si->secondary_namespaces_.size();
    si->get_secondary_namespaces().for_each([&](android_namespace_t *np) {
        STRCAT_MES(np->get_name());
        if (--len != 0) {
            STRCAT_MES(", ");
        }
    });
    STRCAT_HEX("}, handle: ", si->get_handle());
#endif
    return out;
}

bool file_is_in_dir(const std::string &file, const std::string &dir) {
    const char *needle = dir.c_str();
    const char *haystack = file.c_str();
    size_t needle_len = strlen(needle);

    return strncmp(haystack, needle, needle_len) == 0 &&
           haystack[needle_len] == '/' &&
           strchr(haystack + needle_len + 1, '/') == nullptr;
}

bool file_is_under_dir(const std::string &file, const std::string &dir) {
    const char *needle = dir.c_str();
    const char *haystack = file.c_str();
    size_t needle_len = strlen(needle);

    return strncmp(haystack, needle, needle_len) == 0 &&
           haystack[needle_len] == '/';
}

static int _phdr_table_set_load_prot(const ElfW(Phdr) *phdr_table, size_t phdr_count,
                                     ElfW(Addr) load_bias, int extra_prot_flags) {
    const ElfW(Phdr) *phdr = phdr_table;
    const ElfW(Phdr) *phdr_limit = phdr + phdr_count;

    for (; phdr < phdr_limit; phdr++) {
        if (phdr->p_type != PT_LOAD || (phdr->p_flags & PF_W) != 0) {
            continue;
        }

        ElfW(Addr) seg_page_start = PAGE_START(phdr->p_vaddr) + load_bias;
        ElfW(Addr) seg_page_end = PAGE_END(phdr->p_vaddr + phdr->p_memsz) + load_bias;

        int prot = PFLAGS_TO_PROT(phdr->p_flags);
        if ((extra_prot_flags & PROT_WRITE) != 0) {
            // make sure we're never simultaneously writable / executable
            prot &= ~PROT_EXEC;
        }

        int ret = mprotect(reinterpret_cast<void *>(seg_page_start),
                           seg_page_end - seg_page_start,
                           prot | extra_prot_flags);
        if (ret < 0) {
            return -1;
        }
    }
    return 0;
}

int phdr_table_protect_segments(const ElfW(Phdr) *phdr_table, size_t phdr_count, ElfW(Addr) load_bias) {
    return _phdr_table_set_load_prot(phdr_table, phdr_count, load_bias, 0);
}

/* Change the protection of all loaded segments in memory to writable.
 * This is useful before performing relocations. Once completed, you
 * will have to call phdr_table_protect_segments to restore the original
 * protection flags on all segments.
 *
 * Note that some writable segments can also have their content turned
 * to read-only by calling phdr_table_protect_gnu_relro. This is no
 * performed here.
 *
 * Input:
 *   phdr_table  -> program header table
 *   phdr_count  -> number of entries in tables
 *   load_bias   -> load bias
 * Return:
 *   0 on error, -1 on failure (error code in errno).
 */
int phdr_table_unprotect_segments(const ElfW(Phdr) *phdr_table, size_t phdr_count, ElfW(Addr) load_bias) {
    return _phdr_table_set_load_prot(phdr_table, phdr_count, load_bias, PROT_WRITE);
}

uint32_t calculate_elf_hash(const char *name) {
    const uint8_t *name_bytes = (const uint8_t *) name;
    uint32_t h = 0, g;

    while (*name_bytes) {
        h = (h << 4) + *name_bytes++;
        g = h & 0xf0000000;
        h ^= g;
        h ^= g >> 24;
    }
    return h;
}

uint32_t calculate_gnu_hash(const char *name) {
    const uint8_t *name_bytes = (const uint8_t *) name;
    uint32_t h = 5381;

    while (*name_bytes != 0) {
        h += (h << 5) + *name_bytes++;
    }
    return h;
}