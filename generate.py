#!/usr/bin/env python3
import random

PAGE_SIZE=4096
L1I_CACHELINE=64
L1ITLB_ENTRIES=64


class JumpSlot:
    def __init__(self, index, to, disambig, cache_line=L1I_CACHELINE):
        self.section = f".jtab.{disambig}.sec_{index}"
        self.label = f"j_{disambig}_{index}"
        self.to_label = f"j_{disambig}_{to}"
        self.__to = to
        self.rand_align = PAGE_SIZE * random.randint(8,16)

        if cache_line:
            __nr_pads = PAGE_SIZE // cache_line

            n = index % __nr_pads
            self.__pads = ["0"] * (n * cache_line // 4)
        else:
            self.__pads = [ "0" ]

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


def generate(alias, num, noskip_cache=False):
    chain = get_chain(num)
    line  = 0 if noskip_cache else L1I_CACHELINE
    slots = [JumpSlot(i, j, alias, line) for i, j in enumerate(chain)]

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

generate("small", L1ITLB_ENTRIES // 2)
generate("fit",   L1ITLB_ENTRIES, True)
generate("scrap", L1ITLB_ENTRIES * 2)