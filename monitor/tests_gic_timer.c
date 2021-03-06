#include "gic.h"
#include "vgic.h"
#include "context.h"
#include "hvmm_trace.h"
#include "armv7_p15.h"
#include "timer.h"
#include "sched_policy.h"

static void test_start_timer(void)
{
	uint32_t ctl;
	uint32_t tval;
	uint64_t pct;

	HVMM_TRACE_ENTER();


	/* every second */
	tval = read_cntfrq();
	write_cntp_tval(tval);

	pct = read_cntpct();
	uart_print( "cntpct:"); uart_print_hex64(pct); uart_print("\n\r");
	uart_print( "cntp_tval:"); uart_print_hex32(tval); uart_print("\n\r");

	/* enable timer */
	ctl = read_cntp_ctl();
	ctl |= 0x1;
	write_cntp_ctl(ctl);

	HVMM_TRACE_EXIT();
}

void interrupt_nsptimer(int irq, void *pregs, void *pdata )
{
	uint32_t ctl;
	struct arch_regs *regs = pregs;

	uart_print( "=======================================\n\r" );
	HVMM_TRACE_ENTER();

	/* Disable NS Physical Timer Interrupt */
	ctl = read_cntp_ctl();
	ctl &= ~(0x1);
	write_cntp_ctl(ctl);
	

	/* Trigger another interrupt */
	test_start_timer();

	/* Test guest context switch */
	if ( (regs->cpsr & 0x1F) != 0x1A ) {
		/* Not from Hyp, switch the guest context */
		context_dump_regs( regs );
        context_switchto(sched_policy_determ_next());
	}

	HVMM_TRACE_EXIT();
	uart_print( "=======================================\n\r" );
}

hvmm_status_t hvmm_tests_gic_timer(void)
{
	/* Testing Non-secure Physical Timer Event (PPI2, Interrupt ID:30), Cortex-A15 
	 * - Periodically triggers timer interrupt
	 * - switches guest context at every timer interrupt
	 */

	HVMM_TRACE_ENTER();
	/* handler */
	gic_test_set_irq_handler( 30, &interrupt_nsptimer, 0 );

	/* configure and enable interrupt */
	gic_test_configure_irq(30, 
		GIC_INT_POLARITY_LEVEL, 
		gic_cpumask_current(), 
		GIC_INT_PRIORITY_DEFAULT );

	/* start timer */
	test_start_timer();

	HVMM_TRACE_EXIT();
	return HVMM_STATUS_SUCCESS;
}

void callback_timer(void *pdata)
{
    HVMM_TRACE_ENTER();
    vgic_inject_virq_sw( 30, VIRQ_STATE_PENDING, GIC_INT_PRIORITY_DEFAULT, smp_processor_id(), 1);
    HVMM_TRACE_EXIT();
}

hvmm_status_t hvmm_tests_vgic(void)
{
    /* VGIC test 
     *  - Implementation Not Complete
     *  - TODO: specify guest to receive the virtual IRQ
     *  - Once the guest responds to the IRQ, Virtual Maintenance Interrupt service routine should be called
     *      -> ISR implementation is empty for the moment
     *      -> This should handle completion of deactivation and further injection if there is any pending virtual IRQ
     */
    timer_add_callback(timer_sched, &callback_timer);
    return HVMM_STATUS_SUCCESS;
}
