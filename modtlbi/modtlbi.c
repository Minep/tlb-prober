#include <linux/fs.h> 
#include <linux/init.h> 
#include <linux/kobject.h> 
#include <linux/module.h> 
#include <linux/string.h> 
#include <linux/sysfs.h> 

// #define DEBUG
 
static struct kobject *modtlbi; 
 
static ssize_t tlbi_show(struct kobject *kobj, 
                               struct kobj_attribute *attr, char *buf) 
{ 
    return 0; 
} 

static inline void
__tlbi(void)
{
    asm volatile (
        "tlbi vmalle1\n"
        "dsb sy\n"
        "isb\n"
    );
}

static inline void
__tlbi(void)
{
    asm volatile (
        "tlbi vmalle1\n"
        "dsb sy\n"
        "isb\n"
    );
}

static inline void
__tlbi_va(unsigned long va)
{
    asm volatile (
        "tlbi vaae1, %0\n"
        "dsb sy\n"
        "isb\n"
        ::"r"(va >> PAGE_SHIFT)
    );
}

static ssize_t tlbi_store(struct kobject *kobj, 
                                struct kobj_attribute *attr, const char *buf, 
                                size_t count) 
{
    if (!strcmp(buf, "all")) {
#ifdef DEBUG
        pr_info("modtlbi: flush all\n"); 
#endif
        __tlbi();
        return count;
    }

    unsigned long va;
    sscanf(buf, "%lx", &va); 

#ifdef DEBUG
    pr_info("modtlbi: flush va:0x%lx\n", va); 
#endif

    __tlbi_va(va);

    return count; 
} 
 
static struct kobj_attribute tlbi_attribute = 
    __ATTR(tlbi, 0660, tlbi_show, tlbi_store); 
 
static int __init modtlbi_init(void) 
{ 
    int error = 0; 
 
    pr_info("modtlbi: initialized\n"); 
 
    modtlbi = kobject_create_and_add("tlbi", kernel_kobj); 
    if (!modtlbi) 
        return -ENOMEM; 
 
    error = sysfs_create_file(modtlbi, &tlbi_attribute.attr); 
    if (error) { 
        kobject_put(modtlbi); 
        pr_info("failed to create the tlbi file " 
                "in /sys/kernel/tlbi\n"); 
    } 
 
    return error; 
} 
 
static void __exit modtlbi_exit(void) 
{ 
    pr_info("modtlbi: unloaded\n"); 
    kobject_put(modtlbi); 
} 
 
module_init(modtlbi_init); 
module_exit(modtlbi_exit); 
 
MODULE_LICENSE("GPL");
