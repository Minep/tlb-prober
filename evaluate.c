#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <getopt.h>
#include <errno.h>
#include <string.h>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>

#define FLUSH_ALL_CMD "all"

#define NR_DTLB_ENTRIES 224
#define NR_DTLB_WAYS    7

#define NR_SETS     256
#define NR_BLKSZ    64
#define NR_REGIONS  (512)

#define READ_BUFFER_SZ  4096

#define PMU_CPU_CYCLES          0x0011
#define PMU_L2D_TLB_REFILL      0x002d 
#define PMU_L1D_TLB_REFILL      0x0005
#define MAP_OPTS                (MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE)
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
    unsigned long long l1d_refill;
    unsigned long long l2d_refill;
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

static ptr_t
__setup_region(struct eval_context* ctx, ptr_t va, int pgsz)
{
    ptr_t next = 5;
    int nr_ents = pgsz / NR_BLKSZ;
    char* v = (char*)va;
    
    for (int i = 0; i < nr_ents - 1; i++) {
        *(int*)(&v[i * NR_BLKSZ]) = i + 1;
    }

    *(int*)(&v[(nr_ents - 1) * NR_BLKSZ]) = 0;
    for (int i = 0; i < nr_ents; i++) {
        next = next * 1664525UL + 1013904223UL;
        unsigned int r = next % (nr_ents);
        int j = *(int*)(&v[r * NR_BLKSZ]);
        *(int*)(&v[r * NR_BLKSZ]) = *(int*)(&v[i * NR_BLKSZ]);
        *(int*)(&v[i * NR_BLKSZ]) = j;
    }

    for (int i = 0; i < nr_ents - 1; i++) {
        if (*(int*)(&v[i * NR_BLKSZ]) == 1) {
            ctx->start = i;
            break;
        }
    }

    int off = ctx->start, i = 0;
    do {
        off = *(int*)&v[off * NR_BLKSZ];
        i++;
    } while (off);
    
    ctx->expect_nr_insts = i;

    return (ptr_t)v;
}

static void
__prepare_regions(struct eval_context* ctx)
{
    ptr_t va = 0;
    int pgsz = sysconf(_SC_PAGESIZE);
    ptr_t next = 12;
    int dtlb_sets = NR_DTLB_ENTRIES / NR_DTLB_WAYS;

    ctx->p_locs = ensure(mmap, (void*)va, NR_REGIONS * pgsz, PROT_WRITE, MAP_OPTS, 0, 0);

    for (int i = 0; i < NR_REGIONS; ++i)
    {
        va = __setup_region(ctx, va, pgsz);
        ctx->p_locs[i] = va;
        next = next * 1664525UL + 1013904223UL;
        va = va +  pgsz;
    }
}

static inline int force_inline
__run_bench(struct eval_context* ctx, int i)
{
    register int off = ctx->start;
    register char* arr = (char*)ctx->p_locs[i];

    do {
        off = *(int*)(&arr[off * NR_BLKSZ]);
    } while(off);
}

static inline int force_inline
__run_bench_skip_table(struct eval_context* ctx)
{
    register int off = ctx->start;
    register int i = 0;
    char* arr = (char*)ctx->p_locs[i];

    do {
        off = *(int*)(&arr[off * NR_BLKSZ]);
        arr = (char*)ctx->p_locs[++i];
    } while(off);
}

static struct eval_context*
__create_context(struct eval_param* param)
{
    struct eval_context* ctx;

    ctx = calloc(1, sizeof(*ctx));

    ctx->param   = *param;
    ctx->tlbi_fd = ensure(open, "/sys/kernel/tlbi/tlbi", O_RDWR);

    __prepare_regions(ctx);

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
    evt.config      = PMU_L2D_TLB_REFILL;
    ctx->pmu_l2rf   = perf_event_open(&evt, getpid(), param->affinity, ctx->pmu_cycles, 0);

    evt.config      = PMU_L1D_TLB_REFILL;
    ctx->pmu_l1rf   = perf_event_open(&evt, getpid(), param->affinity, ctx->pmu_cycles, 0);
    
    return ctx;
}

static inline void force_inline
__perf_begin(struct eval_context* ctx)
{
    ioctl(ctx->pmu_cycles, PERF_EVENT_IOC_RESET, 0);
    ioctl(ctx->pmu_l1rf, PERF_EVENT_IOC_RESET, 0);
    ioctl(ctx->pmu_l2rf, PERF_EVENT_IOC_RESET, 0);

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
    ctx->stats.l1d_refill += buf.result.value;

    ensure(read, ctx->pmu_l2rf, buf.tmp_buf,   READ_BUFFER_SZ);
    ctx->stats.l2d_refill += buf.result.value;
}

#define perf(ctx, body)      \
    do {                     \
        __perf_begin(ctx);   \
        body;                \
        __perf_end(ctx);     \
    } while(0)

static inline void
__perf_reset_stats(struct eval_context* ctx)
{
    ctx->stats.cycles     = 0ULL;
    ctx->stats.l1d_refill = 0ULL;
    ctx->stats.l2d_refill = 0ULL;
}

static void no_optimize
__run_dtlb_bench(struct eval_context* ctx)
{
    register volatile int a;
    struct pmu_stats base, nomiss, l1miss;
    flush_tlb(ctx);

    // warm up
    perf(ctx, { });

    // correction for noise
    perf(ctx, { });
    __perf_collect(ctx);
    
    log(overhead, 0
            , ctx->stats.cycles    
            , ctx->stats.l1d_refill
            , ctx->stats.l2d_refill);
    base = ctx->stats;
    
    __perf_reset_stats(ctx);

    flush_tlb(ctx);

    // warm up
    perf(ctx, {
        __run_bench_skip_table(ctx);
    });
    
    // no miss
    perf(ctx, {
        __run_bench_skip_table(ctx);
    });
    __perf_collect(ctx);
    
    log(no_miss , ctx->expect_nr_insts
                , ctx->stats.cycles
                , ctx->stats.l1d_refill
                , ctx->stats.l2d_refill);
                
    nomiss = ctx->stats;
    __perf_reset_stats(ctx);

    // l1 miss
    
    perf(ctx, {
        __run_bench_skip_table(ctx);
    });

    flush_tlb(ctx);

    for (int i = 0; i < NR_REGIONS; i++) {
        a = *ctx->c_locs[i];
    }

    perf(ctx, {
        __run_bench_skip_table(ctx);
    });
    __perf_collect(ctx);

    log(l1miss, ctx->expect_nr_insts
              , ctx->stats.cycles
              , ctx->stats.l1d_refill
              , ctx->stats.l2d_refill);

    l1miss = ctx->stats;
    __perf_reset_stats(ctx);


    double tick_pmu, tick_nomiss, tick_l1miss, tick_mmu;
    double tick_l1lat, tick_l2lat, tick_mmu_tot;
    double n_l1, compensate, l1l2;
    
    tick_pmu = (double)base.cycles;
    tick_nomiss = (double)nomiss.cycles;
    tick_l1miss = (double)l1miss.cycles;

    n_l1 = ctx->expect_nr_insts - l1miss.l1d_refill;
    tick_nomiss = (tick_nomiss - tick_pmu) / ctx->expect_nr_insts;
    tick_l1miss = (tick_l1miss - tick_pmu) - tick_nomiss * n_l1;
    tick_l1miss = tick_l1miss / l1miss.l1d_refill;
    tick_mmu = tick_l1miss - tick_nomiss;

    printf("lat-note, inst, iex+lsu, l2_hit\n");
    printf("lat     ,%0.2lf,%0.2lf,%0.2lf\n", 
            tick_l1miss, tick_nomiss, tick_mmu);
}

static void no_optimize
run_bench(struct eval_context* ctx)
{
    srand(0);
    __run_dtlb_bench(ctx);
}

static void
cleanup(struct eval_context* ctx)
{
    close(ctx->tlbi_fd);
    close(ctx->pmu_cycles);
    close(ctx->pmu_l1rf);
    close(ctx->pmu_l2rf);

    int pgsz = sysconf(_SC_PAGESIZE);
    
    for (int i = 0; i < NR_SETS; i++)
    {
        munmap((void*)ctx->p_locs[i], pgsz);
    }
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