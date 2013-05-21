
#include "hyp_config.h"
#include "uart_print.h"
#include "mmu.h"
#include "armv7_p15.h"
#include "arch_types.h"

#define NUM_GUEST_CONTEXTS		NUM_GUESTS_STATIC
#define ARCH_REGS_NUM_GPR	13

typedef enum {
	HYP_RESULT_ERET	= 0,
	HYP_RESULT_STAY = 1
} hyp_hvc_result_t;

struct arch_regs {
        unsigned int cpsr; /* CPSR */
        unsigned int pc; /* Program Counter */
        unsigned int gpr[ARCH_REGS_NUM_GPR]; /* R0 - R12 */
        unsigned int sp; /* Stack Pointer */
        unsigned int lr; /* Link Register */
} __attribute((packed));

struct hyp_guest_context {
	struct arch_regs regs;
	lpaed_t *ttbl;
	vmid_t vmid;
};

extern void __mon_switch_to_guest_context( struct arch_regs *regs );


static struct hyp_guest_context guest_contexts[NUM_GUEST_CONTEXTS];
static int current_guest = 0;
extern void *guest_start;
extern void *guest2_start;


static void hyp_switch_to_next_guest(struct arch_regs *regs_current);

void uart_dump_regs( struct arch_regs *regs ) 
{
	uart_print( "cpsr:" ); uart_print_hex32( regs->cpsr ); uart_print( "\n\r" );
	uart_print( "pc:" ); uart_print_hex32( regs->pc ); uart_print( "\n\r" );
	uart_print( "sp:" ); uart_print_hex32( regs->sp ); uart_print( "\n\r" );
	uart_print( "lr:" ); uart_print_hex32( regs->lr ); uart_print( "\n\r" );
}

hyp_hvc_result_t _hyp_hvc_service(struct arch_regs *regs)
{
	unsigned int hsr = read_hsr();
	unsigned int iss = hsr & 0xFFFF;
	unsigned int ec = (hsr >> 26);
	uart_print("[hvc] _hyp_hvc_service: enter\n\r");

	if ( ec == 0x12 && iss == 0xFFFF ) {
		uart_print("[hvc] enter hyp\n\r");
		uart_dump_regs( regs );
		return HYP_RESULT_STAY;
	}

	switch( iss ) {
		case 0xFFFE:
			// hyp_ping
			uart_print("[hyp] _hyp_hvc_service:ping\n\r");
			uart_dump_regs( regs );
			break;
		case 0xFFFD:
			// hsvc_yield()
			uart_print("[hyp] _hyp_hvc_service:yield\n\r");
			uart_dump_regs( regs );
			hyp_switch_to_next_guest(regs);
			break;
		default:
			uart_print("[hyp] _hyp_hvc_service:unknown iss=");
			uart_print_hex32( iss );
			uart_print("\n\r" );
			uart_dump_regs( regs );
			break;
	}
	uart_print("[hyp] _hyp_hvc_service: done\n\r");
	return HYP_RESULT_ERET;
}


void hyp_init_guests(void)
{
	struct hyp_guest_context *context;
	struct arch_regs *regs = 0;
	
	uart_print("[hyp] init_guests: enter\n\r");

	uart_print("[hyp] init_guests: guest_start");
	uart_print_hex32( (unsigned int) &guest_start); uart_print("\n\r");

	// Guest 1 @guest_start
	context = &guest_contexts[0];
	regs = &context->regs;
	regs->pc = (unsigned int) &guest_start;
	regs->cpsr 	= 0;	// uninitialized
	regs->sp	= 0;	// uninitialized
	regs->lr	= 0;	// uninitialized
	// regs->gpr[] = whatever
	context->vmid = 0;
	context->ttbl = vmm_vmid_ttbl(context->vmid);

	uart_print("[hyp] init_guests: guest2_start");
	uart_print_hex32( (unsigned int) &guest2_start); uart_print("\n\r");

	// Guest 2 @guest2_bin_start
	context = &guest_contexts[1];
	regs = &context->regs;
	regs->pc = (unsigned int) &guest2_start;
	regs->cpsr 	= 0;	// uninitialized
	regs->sp	= 0;	// uninitialized
	regs->lr	= 0;	// uninitialized
	// regs->gpr[] = whatever
	context->vmid = 1;
	context->ttbl = vmm_vmid_ttbl(context->vmid);


	//__mon_install_guest();
	uart_print("[hyp] init_guests: return\n\r");
}

static void hyp_switch_to_next_guest(struct arch_regs *regs_current)
{
	struct hyp_guest_context *context = 0;
	struct arch_regs *regs = 0;
	int i;
	uint32_t hcr;
	
	/*
	 * We assume VTCR has been configured and initialized in the memory management module
	 */

	// Disable Stage 2 Translation: HCR.VM = 0
	vmm_stage2_enable(0);

	if ( regs_current != 0 ) {
		// store
		context = &guest_contexts[current_guest];
		regs = &context->regs;
		regs->pc = regs_current->pc;
		regs->cpsr = regs_current->cpsr;
		regs->sp = regs_current->sp;
		regs->lr = regs_current->lr;
		for( i = 0; i < ARCH_REGS_NUM_GPR; i++) {
			regs->gpr[i] = regs_current->gpr[i];
		}
	}

	if ( regs_current != 0 ) {
		// load the next guest
		current_guest = (current_guest + 1) % NUM_GUEST_CONTEXTS;
	} else {
		// Initial guest
		current_guest = 0;
	}
	context = &guest_contexts[current_guest];

	vmm_set_vmid_ttbl( context->vmid, context->ttbl );

	vmm_stage2_enable(1);
	__mon_switch_to_guest_context( &context->regs );
}

void hyp_switch_to_initial_guest(void)
{
	struct hyp_guest_context *context = 0;
	struct arch_regs *regs = 0;

	uart_print("[hyp] switch_to_initial_guest:\n\r");
	current_guest = 0;
	context = &guest_contexts[current_guest];
	regs = &context->regs;

	uart_dump_regs( regs );
	/*
	__mon_switch_to_guest_context( regs );
	*/
	hyp_switch_to_next_guest( 0 );
}

void hyp_switch_guest(void)
{
	uart_print("[hyp] switch_guest: enter, not coming back\n\r");
	hyp_switch_to_initial_guest();
}

void hyp_main(void)
{
	uart_print("[hyp_main] Starting...\n\r");

	mmu_init();

	hyp_init_guests();
	hyp_switch_guest();

	uart_print("[hyp_main] ERROR: CODE MUST NOT REACH HERE\n\r");
}

