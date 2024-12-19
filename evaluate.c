#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <getopt.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <linux/perf_event.h>

#include "config.h"

#define FLUSH_ALL_CMD "all"

#define READ_BUFFER_SZ  4096

#define PMU_CPU_CYCLES          0x0011
#ifdef ITLB_BENCH
#define PMU_L2_TLB_REFILL       0x002d 
#define PMU_L1_TLB_REFILL       0x0002
#else
#define PMU_L2_TLB_REFILL       0x002d 
#define PMU_L1_TLB_REFILL       0x0005
#endif
#define MAP_OPTS                (MAP_ANONYMOUS | MAP_PRIVATE)
#define true 1

#define no_optimize             __attribute__((optimize("O0")))
#define force_inline            __attribute__((always_inline))

#define ensure(call, ...)                                   \
    ({                                                      \
        long ret = (long)call(__VA_ARGS__);                 \
        if (ret < 0) {                                      \
            printf("failed: " #call "(" #__VA_ARGS__ "), error: %s\n", strerror(errno));   \
            exit(1);                                        \
        }                                                   \
        ret;                                                \
    })

#define log(type, nr_i, cycle, l1, l2)    \
        printf(#type ",%u,%llu,%llu,%llu\n", nr_i, cycle, l1, l2)

typedef unsigned long ptr_t;
typedef unsigned int  inst_t;

struct read_format {
    unsigned long value;     /* The value of the event */
    unsigned long time_enabled;  /* if PERF_FORMAT_TOTAL_TIME_ENABLED */
    unsigned long time_running;  /* if PERF_FORMAT_TOTAL_TIME_RUNNING */
    unsigned long id;        /* if PERF_FORMAT_ID */
    unsigned long lost;      /* if PERF_FORMAT_LOST */
};

struct read_buffer {
    union {
        char tmp_buf[READ_BUFFER_SZ];
        struct read_format result;
    };
};

struct eval_param
{
    int affinity;
    int sample_cnt;
    int nr_skip;
    int mode;
};

struct pmu_stats {
    unsigned long long cycles;
};

struct eval_context
{
    union {
        volatile int** c_locs;
        volatile ptr_t* p_locs;
    };

    int start, expect_nr_insts;
    struct eval_param param;
    
    struct {
        int pmu_cycles;
    };

    struct pmu_stats stats;
};

static int
perf_event_open(struct perf_event_attr* evt, 
                pid_t pid, int cpu, int grp_fd, unsigned long flags)
{
    int ret;

    ret = ensure(syscall, SYS_perf_event_open, evt, pid, cpu, grp_fd, flags);
    
    return ret;
}

static void
__parse_args(struct eval_param* param, int argc, char** argv)
{
    int opti;
    char c;
    struct option opts[] = {
        {"cpu", required_argument, 0, 'c'},
        {"samples", required_argument, 0, 'n'},
        {"mode", required_argument, 0, 'm'},
        {"skip", required_argument, 0, 's'},
        {0, 0, 0, 0} // Terminator
    };
    
    while ((c = getopt_long(argc, argv, "c:n:m:s:", opts, &opti)) != 255)
    {
        switch (c)
        {
            case 'c':
                param->affinity = atoi(optarg);
            break;

            case 'n':
                param->sample_cnt = atoi(optarg);
            break;

            case 'm':
                param->mode = atoi(optarg);
            break;

            case 's':
                param->nr_skip = atoi(optarg);
            break;

            default:
                printf("unknwon args: %d\n", c);
                exit(1);
            break;
        }
    }
}

static int*
__generate_rand_chain(ptr_t seed, int num, int skip)
{
    unsigned long next = seed;
    int seqi_len = num - 1;
    int* arr = calloc(sizeof(int), seqi_len);
    int* chain = calloc(sizeof(int), num);
    int win_sz = seqi_len, remain_sz;

#ifdef ITLB_BENCH
    if (win_sz > 8192 / skip) {
        win_sz = 8192 / skip;
    }
#endif

    for (int i = 0; i < seqi_len; i++) {
        arr[i] = i + 1;
    }

    for (int j = 0; j < seqi_len; j += win_sz) {
        remain_sz = seqi_len - j;
        remain_sz = win_sz < remain_sz ? win_sz : remain_sz;

        for (int i = j; i < win_sz; i++) {
            next = next * 1664525UL + 1013904223UL;
            int r = j + next % (remain_sz);
            int k = arr[i];

            arr[i] = arr[r];
            arr[r] = k;
        }
    }

    int j = 0;
    for (int i = 0; i < seqi_len; i++) {
        chain[j] = arr[i];
        j = arr[i];
    }

    chain[j] = 0;
    free(arr);

    return chain;
}

#define get_offset(i, blksz, nr_slot, pgsz, n)  \
    ((i) * (pgsz) * (n) + (((i) + ((i) / (nr_slot))) % (nr_slot)) * (blksz))

#define inst_b(imm)      ((0b000101 << 26) | ((imm) & ((1 << 26) - 1)))
#define inst_ret         0xd65f03c0
#define abs(x)           ( (x) < 0 ? -(x) : (x) )

static inline void
__set_data(char *region, ptr_t off, ptr_t off_cur)
{
    *(ptr_t*)(&region[off_cur]) = off + (ptr_t)region;
}

static inline void
__set_inst(char *region, ptr_t off, ptr_t off_cur)
{
    inst_t* inst_buffer = (inst_t*)&region[off_cur];
    long dist = (((long)off - (long)off_cur)) / (long)sizeof(inst_t);

    if (off && (abs(dist) >> 25)) {
        printf("immediate too big (0x%lx, 0x%lx -> 0x%lx)\n", abs(dist), off_cur, off);
        _exit(1);
    }

    *inst_buffer = off ? inst_b(dist) : inst_ret;
}

static char*
__setup_page_seq(ptr_t seed, int num, int with_cacheline, int nr_skips, ptr_t* size)
{
    int pgsz, *chain, nr_slot;
    char *region;
    unsigned long next = seed + 42;

    pgsz = sysconf(_SC_PAGESIZE);
    chain = __generate_rand_chain(seed, num, nr_skips);

    unsigned long sz = num * pgsz * nr_skips;
    region = (char*)ensure(mmap, NULL, sz, PROT_WRITE, MAP_OPTS, 0, 0);
    madvise(region, sz, MADV_UNMERGEABLE | MADV_NOHUGEPAGE);

    int index = 0;
    int blksz = !with_cacheline ? 0 : L1DCACHE_LINES;
    ptr_t off = 0, off_cur;
    nr_slot   = pgsz / L1DCACHE_LINES;
    for (int i = 0; i < num; i++)
    {
        index = chain[i];

        off   = get_offset(index, blksz, nr_slot, pgsz, nr_skips);
        off_cur = get_offset(i, blksz, nr_slot, pgsz, nr_skips);
        
#ifdef ITLB_BENCH
        __set_inst(region, off, off_cur);
#else
        __set_data(region, off, off_cur);
#endif
    }

#ifdef ITLB_BENCH
    ensure(mprotect, region, sz, PROT_EXEC);
#endif
    
    free(chain);
    *size = sz;
    return region;
}

#define OP      "ldr x0, [x0]\n"
#define OP4      OP OP OP OP
#define OP16     OP4 OP4 OP4 OP4
#define OP32     OP16 OP16
#define OP64     OP32 OP32
#define OP128    OP64 OP64
#define OP256    OP128 OP128
#define OP512    OP256 OP256
#define OP1024   OP512 OP512
#define OP2048   OP1024 OP1024

#define NR_ACCESS 2048

static inline int force_inline
__run_bench(ptr_t* off)
{
    register ptr_t *off_o, *off_;

    off_o = off;
    off_  = off;
    do {
        off_ = *(ptr_t**)off_;
    } while(off_o != off_);
}

static struct eval_context*
__create_context(struct eval_param* param)
{
    struct eval_context* ctx;

    ctx = calloc(1, sizeof(*ctx));

    ctx->param   = *param;

    struct perf_event_attr evt;
    memset(&evt, 0, sizeof(evt));

    evt.disabled = true;
    evt.exclude_hv = true;
    evt.exclude_kernel = true;
    evt.size = sizeof(evt);

    evt.type        = PERF_TYPE_HARDWARE;
    evt.config      = PERF_COUNT_HW_CPU_CYCLES;
    evt.pinned      = true;
    ctx->pmu_cycles = perf_event_open(&evt, getpid(), param->affinity, -1, 0);

    return ctx;
}

static inline void force_inline
__perf_begin(struct eval_context* ctx)
{
    ioctl(ctx->pmu_cycles, PERF_EVENT_IOC_RESET, 0);
    ioctl(ctx->pmu_cycles, PERF_EVENT_IOC_ENABLE, 0);
}

static inline void force_inline
__perf_end(struct eval_context* ctx)
{
    ioctl(ctx->pmu_cycles, PERF_EVENT_IOC_DISABLE, 0);
}

static void
__perf_collect(struct eval_context* ctx)
{
    struct read_buffer buf;

    ensure(read, ctx->pmu_cycles, buf.tmp_buf, READ_BUFFER_SZ);
    ctx->stats.cycles = buf.result.value;
}

#define perf(ctx, body)      \
    do {                     \
        __perf_begin(ctx);   \
        body;                \
        __perf_end(ctx);     \
    } while(0)

static void no_optimize
__run_tlb_bench_seq(struct eval_context* ctx, int n)
{
    char *data_small;
    int base;
    register ptr_t *data_small_first;
    ptr_t sz;
    register int i = 0;
    struct pmu_stats l1miss;

    data_small = __setup_page_seq(43, n, true, ctx->param.nr_skip, &sz);
    data_small_first = (ptr_t*)(*((ptr_t*)data_small));

    perf(ctx, { });

    // // correction for noise
    perf(ctx, { });
    __perf_collect(ctx);
    base = ctx->stats.cycles;

#ifndef ITLB_BENCH
        __run_bench(data_small_first);
#else
        asm volatile ("blr %0\n" ::"r"(data_small));
#endif
    
    perf(ctx, {
        while(i++ < 10000) {
#ifndef ITLB_BENCH
        __run_bench(data_small_first);
#else
        asm volatile ("blr %0\n" ::"r"(data_small));
#endif
        }

    });
    __perf_collect(ctx);

    l1miss = ctx->stats;

    double inst_cycle;

    inst_cycle = ((double)l1miss.cycles - base) / n;
    printf("%0.4lf\n", inst_cycle / 10000.0);
    munmap(data_small, sz);
}

static void
cleanup(struct eval_context* ctx)
{
    close(ctx->pmu_cycles);
}

#define KiB 1024
#define MiB KiB * KiB
#define GiB MiB * KiB

int
main(int argc, char** argv)
{
    struct eval_param param;
    struct eval_context* ctx;
    int valid;

    struct rlimit lim = {
        .rlim_cur = 1 * GiB,
        .rlim_max = 1 * GiB
    };

    setrlimit(RLIMIT_DATA, &lim);

    __parse_args(&param, argc, argv);

    ctx = __create_context(&param);

    for (int i = 0; i < ctx->param.sample_cnt;i++)
    {
        if (!param.mode) {
            __run_tlb_bench_seq(ctx, 1 << i);
        }
        else {
            __run_tlb_bench_seq(ctx, (i+1) * param.mode);
        }
    }
    cleanup(ctx);
}