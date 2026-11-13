
deps/usr/lib/librte_net_ring.so:     file format elf64-x86-64


Disassembly of section .text:

0000000000002fa2 <rte_eth_from_rings@@DPDK_26-0x163e>:
    2fa2:	44 89 b7 04 01 00 00 	mov    %r14d,0x104(%rdi)
    2fa9:	48 8b 5d e8          	mov    -0x18(%rbp),%rbx
    2fad:	48 89 f0             	mov    %rsi,%rax
    2fb0:	41 0f b7 78 08       	movzwl 0x8(%r8),%edi
    2fb5:	89 d6                	mov    %edx,%esi
    2fb7:	41 89 d1             	mov    %edx,%r9d
    2fba:	48 8d 0c f0          	lea    (%rax,%rsi,8),%rcx
    2fbe:	66 90                	xchg   %ax,%ax
    2fc0:	48 8b 10             	mov    (%rax),%rdx
    2fc3:	48 83 c0 08          	add    $0x8,%rax
    2fc7:	66 89 7a 16          	mov    %di,0x16(%rdx)
    2fcb:	48 39 c8             	cmp    %rcx,%rax
    2fce:	75 f0                	jne    2fc0 <DPDK_26@@DPDK_26+0x2fc0>
