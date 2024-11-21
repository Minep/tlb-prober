#include <stdio.h>
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
#define MAX_SCATTER 256

#define SCATTERS_RANGE  1024
#define READ_BUFFER_SZ  1024
#define MIN_SCATTER     8

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

struct eval_context
{
    union {
        int** c_locs;
        ptr_t* p_locs;
    };

    struct eval_param param;
    
    struct {
        int tlbi_fd;
        int pmu_cycles;
        int pmu_l2rf;
        int pmu_l1rf;
    };

    struct {
        unsigned long long cycles;
        unsigned long long l1d_refill;
        unsigned long long l2d_refill;
    } stats;
};

static void 
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
        {"scatter", required_argument, 0, 's'},
        {0, 0, 0, 0} // Terminator
    };
    
    while ((c = getopt_long(argc, argv, "c:n:s:", opts, &opti)) != 255)
    {
        switch (c)
        {
            case 'c':
                param->affinity = atoi(optarg);
            break;

            case 's':
            {
                int nr = atoi(optarg);
                if (nr < 0) {
                    printf("invalid scatter count: %d\n", nr);
                    exit(1);
                }

                if (nr > MAX_SCATTER) {
                    nr = MAX_SCATTER;
                }

                if (nr < MIN_SCATTER) {
                    nr = MIN_SCATTER;
                }

                param->nr_scatter = nr;
            }
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

static void
__prepare_regions(struct eval_context* ctx)
{
    ptr_t va = 0;
    int pgsz = sysconf(_SC_PAGESIZE);
    int scatter = MAX_SCATTER;

    ctx->p_locs = (ptr_t*)calloc(sizeof(ptr_t), scatter);

    for (int i = 0; i < scatter; ++i)
    {
        if (va) {
            va += ((rand() % SCATTERS_RANGE) - (SCATTERS_RANGE / 2)) * pgsz;
        }
        
        va = (ptr_t)ensure(mmap, (void*)va, pgsz, PROT_WRITE, MAP_OPTS, 0, 0);
        ctx->p_locs[i] = va;
    }
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

    evt.type = PERF_TYPE_RAW;
    evt.disabled = true;
    evt.exclude_hv = true;
    evt.exclude_kernel = true;
    evt.size = sizeof(evt);

    evt.config      = PMU_CPU_CYCLES;
    evt.pinned = true;
    ctx->pmu_cycles = perf_event_open(&evt, getpid(), param->affinity, -1, 0);

    evt.pinned = 0;
    evt.config      = PMU_L2D_TLB_REFILL;
    ctx->pmu_l2rf   = perf_event_open(&evt, getpid(), param->affinity, ctx->pmu_cycles, 0);

    evt.config      = PMU_L1D_TLB_REFILL;
    ctx->pmu_l1rf   = perf_event_open(&evt, getpid(), param->affinity, ctx->pmu_cycles, 0);
    
    return ctx;
}


#define BENCH_BODY(arr, scatter, repeat)                        \
    do {                                                        \
        for (register int i = 0; i < repeat * scatter; ++i) {   \
            *arr[i % scatter] = i;                              \
        }                                                       \
    } while(0)

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

#define log(type, cycle, l1, l2)    \
        printf(#type ",%llu,%llu,%llu\n", cycle, l1, l2)


#define CALIBRATE_REPEAT      10

static void
run_bench(struct eval_context* ctx)
{
    register int repeat, scatter;
    register int** arr;
    
    repeat  = ctx->param.sample_cnt;
    scatter = ctx->param.nr_scatter;
    arr     = ctx->c_locs;

    flush_tlb(ctx);

    // warm up
    perf(ctx, {
        BENCH_BODY(arr, MIN_SCATTER, 2);
    });

    // calibrate for noise
    perf(ctx, {
        BENCH_BODY(arr, MIN_SCATTER, CALIBRATE_REPEAT);
    });
    
    __perf_collect(ctx);
    log(base, ctx->stats.cycles / CALIBRATE_REPEAT
            , ctx->stats.l1d_refill
            , ctx->stats.l2d_refill);
    
    __perf_reset_stats(ctx);

    // collect latency of l1 miss but l2 hit.

    flush_tlb(ctx);

    // warm up
    perf(ctx, {
        BENCH_BODY(arr, MAX_SCATTER, 1);
    });

    perf(ctx, {
        BENCH_BODY(arr, MIN_SCATTER * 2, 1);
    });

    __perf_collect(ctx);
    log(l1miss, ctx->stats.cycles
              , ctx->stats.l1d_refill
              , ctx->stats.l2d_refill);
    __perf_reset_stats(ctx);

    flush_tlb(ctx);

    perf(ctx, { });     // warm up the tlb path for syscalls

    perf(ctx, {
        BENCH_BODY(arr, scatter, 1);
    });

    __perf_collect(ctx);
    log(eval  , ctx->stats.cycles
              , ctx->stats.l1d_refill
              , ctx->stats.l2d_refill);
    __perf_reset_stats(ctx);
}

int no_optimize
main(int argc, char** argv)
{
    struct eval_param param;
    struct eval_context* ctx;

    srand(time(NULL));
    __parse_args(&param, argc, argv);

    ctx = __create_context(&param);
    
    for (int i = 0; i < ctx->param.sample_cnt; i++)
    {
        run_bench(ctx);
    }
    
}