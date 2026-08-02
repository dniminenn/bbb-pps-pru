/* intc_map_0.h
 * INTC interrupt map for ARM host to PRU0 kick interrupt.
 */

/* The kernel's pru_rproc loader reads the INTC mapping from a .pru_irq_map
 * ELF section; without the DATA_SECTION pragma the struct lands in .data and
 * the loader fails the boot with "header-less .pru_irq_map section". */
#pragma DATA_SECTION(my_irq_rsc, ".pru_irq_map")
#pragma RETAIN(my_irq_rsc)
struct pru_irq_rsc my_irq_rsc = {
    0, /* type */
    1, /* 1 sysevt mapped */
    {
        {17, 0, 0}, /* sysevt 17 → channel 0 → host 0 */
    },
};
