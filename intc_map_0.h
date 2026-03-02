/* intc_map_0.h
 * INTC interrupt map for ARM host to PRU0 kick interrupt.
 */

struct pru_irq_rsc my_irq_rsc = {
    0, /* type */
    1, /* 1 sysevt mapped */
    {
        {17, 0, 0}, /* sysevt 17 → channel 0 → host 0 */
    },
};
