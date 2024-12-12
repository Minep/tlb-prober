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
#include <linux/perf_event.h>

#define FLUSH_ALL_CMD "all"

#define NR_DTLB_ENTRIES 224
#define NR_DTLB_WAYS    7

#define NR_SETS     256
#define NR_BLKSZ    64
#define NR_REGIONS  (NR_DTLB_ENTRIES * 2)

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
    int nr_scatter;
};

struct pmu_stats {
    unsigned long long cycles;
    unsigned long long l1_refill;
    unsigned long long l2_refill;
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
        int tlbi_fd;
        int pmu_cycles;
        int pmu_l2rf;
        int pmu_l1rf;
    };

    struct pmu_stats stats;
};

static inline void 
flush_tlb(struct eval_context* ctx)
{
    write(ctx->tlbi_fd, FLUSH_ALL_CMD, sizeof(FLUSH_ALL_CMD));
}

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
        {0, 0, 0, 0} // Terminator
    };
    
    while ((c = getopt_long(argc, argv, "c:n:", opts, &opti)) != 255)
    {
        switch (c)
        {
            case 'c':
                param->affinity = atoi(optarg);
            break;

            case 'n':
                param->sample_cnt = atoi(optarg);
            break;

            default:
                printf("unknwon args: %d\n", c);
                exit(1);
            break;
        }
    }
}

static int*
__generate_rand_chain(ptr_t seed, int num)
{
    unsigned long next = seed;
    int seqi_len = num - 1;
    int* arr = calloc(sizeof(int), seqi_len);
    int* chain = calloc(sizeof(int), num);

    for (int i = 0; i < seqi_len; i++) {
        arr[i] = i + 1;
    }

    for (int i = 0; i < seqi_len; i++) {
        next = next * 1664525UL + 1013904223UL;
        int r = next % (seqi_len);
        int k = arr[i];

        arr[i] = arr[r];
        arr[r] = k;
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

static char*
__setup_page_seq(ptr_t seed, int num, int with_cacheline)
{
    int pgsz, *chain, nr_slot;
    char *region;
    int min_skip = 0, max_skip = 16, rand_skip = 0, prev_skip = 0;
    unsigned long next = seed + 42;

    pgsz = sysconf(_SC_PAGESIZE);
    chain = __generate_rand_chain(seed, num);

    unsigned long sz = num * pgsz;
    region = (char*)ensure(mmap, NULL, sz, PROT_WRITE, MAP_OPTS, 0, 0);
    ensure(madvise, region, sz, MADV_UNMERGEABLE | MADV_NOHUGEPAGE);

    int index = 0;
    int blksz = !with_cacheline ? 0 : NR_BLKSZ;
    ptr_t off = 0;
    nr_slot   = pgsz / NR_BLKSZ;
    for (int i = 0; i < num; i++)
    {
        index = chain[i];
        next = next * 1664525UL + 1013904223UL;
        rand_skip = (next % min_skip) + min_skip;

        off   = index % nr_slot;
        off   = off * blksz + (index + min_skip) * pgsz;
        
        *(ptr_t*)(&region[(i + min_skip) * pgsz + (i % nr_slot) * blksz]) = off;
    }
    
    free(chain);
    return region;
}

static inline int force_inline
__run_bench(char* arr)
{
    register ptr_t off = 0;
    do {
        off = *(ptr_t*)(&arr[off]);
    } while(off);
}

static struct eval_context*
__create_context(struct eval_param* param)
{
    struct eval_context* ctx;

    ctx = calloc(1, sizeof(*ctx));

    ctx->param   = *param;
    ctx->tlbi_fd = ensure(open, "/sys/kernel/tlbi/tlbi", O_RDWR);

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

    evt.pinned = 0;
    evt.type        = PERF_TYPE_RAW;
    evt.config      = PMU_L2_TLB_REFILL;
    ctx->pmu_l2rf   = perf_event_open(&evt, getpid(), param->affinity, -1, 0);

    evt.config      = PMU_L1_TLB_REFILL;
    ctx->pmu_l1rf   = perf_event_open(&evt, getpid(), param->affinity, -1, 0);
    
    return ctx;
}

static inline void force_inline
__perf_begin(struct eval_context* ctx)
{
    ioctl(ctx->pmu_cycles, PERF_EVENT_IOC_RESET, 0);
    ioctl(ctx->pmu_l2rf, PERF_EVENT_IOC_RESET, 0);
    ioctl(ctx->pmu_l1rf, PERF_EVENT_IOC_RESET, 0);

    ioctl(ctx->pmu_l1rf, PERF_EVENT_IOC_ENABLE, 0);
    ioctl(ctx->pmu_l2rf, PERF_EVENT_IOC_ENABLE, 0);
    ioctl(ctx->pmu_cycles, PERF_EVENT_IOC_ENABLE, 0);
}

static inline void force_inline
__perf_end(struct eval_context* ctx)
{
    ioctl(ctx->pmu_cycles, PERF_EVENT_IOC_DISABLE, 0);
    ioctl(ctx->pmu_l1rf, PERF_EVENT_IOC_DISABLE, 0);
    ioctl(ctx->pmu_l2rf, PERF_EVENT_IOC_DISABLE, 0);
}

static void
__perf_collect(struct eval_context* ctx)
{
    struct read_buffer buf;

    ensure(read, ctx->pmu_cycles, buf.tmp_buf, READ_BUFFER_SZ);
    ctx->stats.cycles += buf.result.value;

    ensure(read, ctx->pmu_l1rf, buf.tmp_buf,   READ_BUFFER_SZ);
    ctx->stats.l1_refill += buf.result.value;

    ensure(read, ctx->pmu_l2rf, buf.tmp_buf,   READ_BUFFER_SZ);
    ctx->stats.l2_refill += buf.result.value;
}

static inline void
__perf_reset_stats(struct eval_context* ctx)
{
    ctx->stats.cycles     = 0ULL;
    ctx->stats.l1_refill = 0ULL;
    ctx->stats.l2_refill = 0ULL;
}

#define perf(ctx, body)      \
    do {                     \
        __perf_begin(ctx);   \
        body;                \
        __perf_end(ctx);     \
    } while(0)

#ifdef ITLB_BENCH

#define NR_ACCESS (32)

extern void do_jump_fit();
extern void do_jump_small();
extern void do_jump_scrap();

static void no_optimize
__run_tlb_bench(struct eval_context* ctx)
{
    register int pgsz = sysconf(_SC_PAGESIZE);
    struct pmu_stats base, nomiss, l1miss;
    int valid = 1;

    do_jump_scrap();

    // warm up
    perf(ctx, { });

    // correction for noise
    perf(ctx, { });
    __perf_collect(ctx);
    
    log(overhead, 0
            , ctx->stats.cycles    
            , ctx->stats.l1_refill
            , ctx->stats.l2_refill);
    base = ctx->stats;
    
    __perf_reset_stats(ctx);

    // warm up
    perf(ctx, {
        do_jump_small();
    });
    
    // no miss
    perf(ctx, {
        do_jump_small();
    });
    __perf_collect(ctx);
    
    log(no_miss , NR_ACCESS
                , ctx->stats.cycles
                , ctx->stats.l1_refill
                , ctx->stats.l2_refill);
    nomiss = ctx->stats;
    valid = valid && !nomiss.l1_refill && !nomiss.l2_refill;
    __perf_reset_stats(ctx);

    do_jump_scrap();

    // l1 miss
    
    perf(ctx, {
        do_jump_small();
    });

    do_jump_fit();

    perf(ctx, {
        do_jump_small();
    });
    __perf_collect(ctx);

    log(l1miss, NR_ACCESS
              , ctx->stats.cycles
              , ctx->stats.l1_refill
              , ctx->stats.l2_refill);

    l1miss = ctx->stats;
    valid = valid && l1miss.l1_refill && !l1miss.l2_refill;
    __perf_reset_stats(ctx);

    double tick_pmu, tick_nomiss, tick_l1miss, tick_mmu;
    double tick_l1lat, tick_l2lat, tick_mmu_tot;
    double n_l1, compensate, l1l2;
    
    tick_pmu = (double)base.cycles;
    tick_nomiss = (double)nomiss.cycles;
    tick_l1miss = (double)l1miss.cycles;

    n_l1 = NR_ACCESS - l1miss.l1_refill;
    tick_nomiss = (tick_nomiss - tick_pmu) / NR_ACCESS;
    tick_l1miss = (tick_l1miss - tick_pmu - n_l1 * tick_nomiss) / l1miss.l1_refill;

    tick_mmu = tick_l1miss - tick_nomiss;

    printf("lat-note, inst, iex+lsu, l2_hit, may_invl\n");
    printf("lat     ,%0.2lf,%0.2lf,%0.2lf,%d\n", 
            tick_l1miss, tick_nomiss, tick_mmu, !valid);
}
#else

#define NR_ACCESS (64)
#define NR_SCRAP  (NR_DTLB_ENTRIES * NR_DTLB_WAYS)

static void no_optimize
__run_tlb_bench(struct eval_context* ctx)
{
    register char *data, *data_small, *scrap;
    register int pgsz = sysconf(_SC_PAGESIZE);
    struct pmu_stats base, nomiss, l1miss;
    int valid = 1;

    scrap = __setup_page_seq(11, NR_SCRAP, true);
    data = __setup_page_seq(43, NR_DTLB_ENTRIES, 0);
    data_small = __setup_page_seq(97, NR_ACCESS, true);

    __run_bench(scrap);

    // warm up
    perf(ctx, { });

    // correction for noise
    perf(ctx, { });
    __perf_collect(ctx);
    
    log(overhead, 0
            , ctx->stats.cycles    
            , ctx->stats.l1_refill
            , ctx->stats.l2_refill);
    base = ctx->stats;
    
    __perf_reset_stats(ctx);

    // warm up
    perf(ctx, {
        __run_bench(data_small);
    });
    
    // no miss
    perf(ctx, {
        __run_bench(data_small);
    });
    __perf_collect(ctx);
    
    log(no_miss , NR_ACCESS
                , ctx->stats.cycles
                , ctx->stats.l1_refill
                , ctx->stats.l2_refill);
    nomiss = ctx->stats;
    valid = valid && !nomiss.l1_refill && !nomiss.l2_refill;
    __perf_reset_stats(ctx);

    __run_bench(scrap);

    // l1 miss
    
    perf(ctx, {
        __run_bench(data_small);
    });

    __run_bench(data);

    perf(ctx, {
        __run_bench(data_small);
    });
    __perf_collect(ctx);

    log(l1miss, NR_ACCESS
              , ctx->stats.cycles
              , ctx->stats.l1_refill
              , ctx->stats.l2_refill);

    l1miss = ctx->stats;
    valid = valid && l1miss.l1_refill && !l1miss.l2_refill;
    __perf_reset_stats(ctx);


    double tick_pmu, tick_nomiss, tick_l1miss, tick_mmu;
    double tick_l1lat, tick_l2lat, tick_mmu_tot;
    double n_l1, compensate, l1l2;
    
    tick_pmu = (double)base.cycles;
    tick_nomiss = (double)nomiss.cycles;
    tick_l1miss = (double)l1miss.cycles;

    n_l1 = NR_ACCESS - l1miss.l1_refill;
    tick_nomiss = (tick_nomiss - tick_pmu) / NR_ACCESS;
    tick_l1miss = (tick_l1miss - tick_pmu - n_l1 * tick_nomiss) / l1miss.l1_refill;

    tick_mmu = tick_l1miss - tick_nomiss;

    printf("lat-note, inst, iex+lsu, l2_hit, may_invl\n");
    printf("lat     ,%0.2lf,%0.2lf,%0.2lf,%d\n", 
            tick_l1miss, tick_nomiss, tick_mmu, !valid);
}
#endif

static void no_optimize
run_bench(struct eval_context* ctx)
{
    __run_tlb_bench(ctx);
}

static void
cleanup(struct eval_context* ctx)
{
    close(ctx->tlbi_fd);
    close(ctx->pmu_cycles);
    close(ctx->pmu_l1rf);
    close(ctx->pmu_l2rf);
}

int no_optimize
main(int argc, char** argv)
{
    struct eval_param param;
    struct eval_context* ctx;

    srand(0);
    __parse_args(&param, argc, argv);

    ctx = __create_context(&param);
    
    for (int i = 0; i < ctx->param.sample_cnt; i++)
    {
        run_bench(ctx);
    }
    
    cleanup(ctx);
}