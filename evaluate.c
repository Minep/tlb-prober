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
#define SCATTER_RANGE 512
#define READ_BUFFER_SZ 1024

#define PMU_CPU_CYCLES          0x0011
#define PMU_L2D_TLB_REFILL      0x0005
#define PMU_L1D_TLB_REFILL      0x002d
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

                if (nr > 128) {
                    printf("scatter capped to 128\n");
                    nr = 128;
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

    ctx->p_locs = (ptr_t*)calloc(sizeof(ptr_t), ctx->param.nr_scatter);

    for (int i = 0; i < ctx->param.nr_scatter; ++i)
    {
        if (va) {
            va += ((rand() % SCATTER_RANGE) - (SCATTER_RANGE / 2)) * pgsz;
        }
        
        va = (ptr_t)ensure(mmap, (void*)va, pgsz, PROT_WRITE, MAP_OPTS, 0, 0);
        ctx->p_locs[i] = va;
    }
}

static struct eval_context*
__create_context(struct eval_param* param)
{
    struct eval_context* ctx;

    ctx = malloc(sizeof(*ctx));

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

static inline void force_inline no_optimize
__run_experiments(struct eval_context* ctx)
{
    register int n = ctx->param.nr_scatter;
    register int** arr = ctx->c_locs;

    for (int i = 0; i < n; i++)
    {
        *arr[i] = i;
    }
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

static void
__perf_print_and_reset(struct eval_context* ctx)
{
    printf("%llu,%llu,%llu\n", ctx->stats.cycles
                             , ctx->stats.l1d_refill
                             , ctx->stats.l2d_refill);

    ctx->stats.cycles     = 0ULL;
    ctx->stats.l1d_refill = 0ULL;
    ctx->stats.l2d_refill = 0ULL;
}

#define RUN_EXP(ctx)                    \
    do {                                \
        __perf_begin(ctx);              \
        for (register int i = 0; i < n; ++i) {   \
            __run_experiments(ctx);     \
        }                               \
        __perf_end(ctx);                \
    } while(0)

int 
main(int argc, char** argv)
{
    struct eval_param param;
    struct eval_context* ctx;

    srand(time(NULL));
    __parse_args(&param, argc, argv);

    ctx = __create_context(&param);
    register int i = 0;
    register int n = ctx->param.sample_cnt;

    flush_tlb(ctx);

    RUN_EXP(ctx);
    RUN_EXP(ctx);
    RUN_EXP(ctx);
    
    printf("b,");
    __perf_collect(ctx);
    __perf_print_and_reset(ctx);


    for (i = 0; i < n; ++i) {
        flush_tlb(ctx);
        
        RUN_EXP(ctx);

        printf("%d,", i);
        __perf_collect(ctx);
        __perf_print_and_reset(ctx);
    }
}