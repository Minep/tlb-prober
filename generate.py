#!/usr/bin/env python3
import random
import os

class Config:
    def __init__(self):
        self.l1dtlb_ents     = int(os.environ.get("L1DTLB_ENTRIES", 64))
        self.l1dtlb_ways     = int(os.environ.get("L1DTLB_WAYS",    1))
        self.l1itlb_ents     = int(os.environ.get("L1ITLB_ENTRIES", 64))
        self.l1icache_line   = int(os.environ.get("L1ICACHE_LINES", 64))
        self.l1dcache_line   = int(os.environ.get("L1DCACHE_LINES", 64))
        self.page_size       = int(os.environ.get("PAGE_SIZE",      4096))
        self.randskip_min    = int(os.environ.get("RAND_SKIP_MIN ", 8))
        self.randskip_max    = int(os.environ.get("RAND_SKIP_MAX ", 16))

    def gen_header(self):
        with open("config.h", 'w') as f:
            f.write("#define L1DTLB_ENTRIES %d\n"%(self.l1dtlb_ents  ))
            f.write("#define L1DTLB_WAYS    %d\n"%(self.l1dtlb_ways  ))
            f.write("#define L1ITLB_ENTRIES %d\n"%(self.l1itlb_ents  ))
            f.write("#define L1ICACHE_LINES %d\n"%(self.l1icache_line))
            f.write("#define L1DCACHE_LINES %d\n"%(self.l1dcache_line))
            f.write("#define PAGE_SIZE      %d\n"%(self.page_size    ))
            f.write("#define RAND_SKIP_MIN  %d\n"%(self.randskip_min ))
            f.write("#define RAND_SKIP_MAX  %d\n"%(self.randskip_max ))

class JumpSlot:
    def __init__(self, config, index, to, disambig, spam_cache=True, spam_tlb=True):
        self.section = f".jtab.{disambig}.sec_{index}"
        self.label = f"j_{disambig}_{index}"
        self.to_label = f"j_{disambig}_{to}"
        self.__to = to
        self.config = config
        self.rand_align  = config.page_size
        
        if (spam_tlb):
            self.rand_align *= 16

        self.__pads = ["0"]
        if spam_cache:
            __nr_pads = config.page_size // config.l1icache_line
            n = index % __nr_pads
            self.__pads *= (n * config.l1icache_line // 4)

    def get_insts(self):
        if not self.__to:
            return "ret"
        return f"b {self.to_label}"
    
    def emit(self):
        return "\n".join([
            f".section {self.section}, \"ax\", @progbits",
            f"    .long {','.join(self.__pads)}",
            f"    {self.label}:",
            f"        {self.get_insts()}",
             ""
        ])
    
    def emit_ld(self):
        return "\n".join([
            f"{self.section} ALIGN({self.rand_align}):",
            "{",
            f"    *({self.section});",
            "}",
            ""
        ])

def get_chain(num):
    index = list(range(1,num))
    chain = list(range(0,num))

    for i in range(len(index)):
        r = random.randint(0, len(index)-1)
        a = index[i]
        index[i] = index[r]
        index[r] = a

    a = 0
    for x in index:
        chain[a] = x
        a = x

    chain[a] = 0
    return chain


def generate_jmptab(config, alias, num, spam_cache=True, spam_tlb=True):
    chain = get_chain(num)
    slots = [JumpSlot(config, i, j, alias, spam_cache, spam_tlb) for i, j in enumerate(chain)]

    with open(f"jump_table_{alias}.S", 'w') as f:
        f.write("\n".join([
             ".section .text",
            f"   .globl do_jump_{alias}",
            f"   do_jump_{alias}:",
            f"      b {slots[0].label}",
             ""
        ]))
        for s in slots:
            f.write(s.emit())

    with open(f"jump_table_{alias}.ldx", 'w') as f:
        for s in slots:
            f.write(s.emit_ld())

config = Config()

# generate_jmptab(config, "small", 32, spam_tlb=True)
# generate_jmptab(config, "fit",   32, spam_tlb=False)
# generate_jmptab(config, "scrap", 32)
config.gen_header()