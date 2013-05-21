
#include "hyp_config.h"
#include "mmu.h"
#include "uart_print.h"
#include "armv7_p15.h"
#include "arch_types.h"

/* LPAE Memory region attributes, to match Linux's (non-LPAE) choices.
 * Indexed by the AttrIndex bits of a LPAE entry;
 * the 8-bit fields are packed little-endian into MAIR0 and MAIR1
 *
 *                 ai    encoding
 *   UNCACHED      000   0000 0000  -- Strongly Ordered
 *   BUFFERABLE    001   0100 0100  -- Non-Cacheable
 *   WRITETHROUGH  010   1010 1010  -- Write-through
 *   WRITEBACK     011   1110 1110  -- Write-back
 *   DEV_SHARED    100   0000 0100  -- Device
 *   ??            101
 *   reserved      110
 *   WRITEALLOC    111   1111 1111  -- Write-back write-allocate
 *      
 *   DEV_NONSHARED 100   (== DEV_SHARED)
 *   DEV_WC        001   (== BUFFERABLE)
 *   DEV_CACHED    011   (== WRITEBACK)
 */ 
#define INITIAL_MAIR0VAL 0xeeaa4400
#define INITIAL_MAIR1VAL 0xff000004
#define INITIAL_MAIRVAL (INITIAL_MAIR0VAL|INITIAL_MAIR1VAL<<32)

/* SCTLR System Control Register. */
/* HSCTLR is a subset of this. */
#define SCTLR_TE        (1<<30)
#define SCTLR_AFE       (1<<29)
#define SCTLR_TRE       (1<<28)
#define SCTLR_NMFI      (1<<27)
#define SCTLR_EE        (1<<25)
#define SCTLR_VE        (1<<24)
#define SCTLR_U         (1<<22)
#define SCTLR_FI        (1<<21)
#define SCTLR_WXN       (1<<19)
#define SCTLR_HA        (1<<17)
#define SCTLR_RR        (1<<14)
#define SCTLR_V         (1<<13)
#define SCTLR_I         (1<<12)
#define SCTLR_Z         (1<<11)
#define SCTLR_SW        (1<<10)
#define SCTLR_B         (1<<7)
#define SCTLR_C         (1<<2)
#define SCTLR_A         (1<<1)
#define SCTLR_M         (1<<0)
#define SCTLR_BASE        0x00c50078
#define HSCTLR_BASE       0x30c51878
/* HTTBR */
#define HTTBR_INITVAL                                   0x0000000000000000ULL
#define HTTBR_BADDR_MASK                                0x000000FFFFFFF000ULL
#define HTTBR_BADDR_SHIFT                               12

/* VTTBR */ 
#define VTTBR_INITVAL                                   0x0000000000000000ULL
#define VTTBR_VMID_MASK                                 0x00FF000000000000ULL
#define VTTBR_VMID_SHIFT                                48
#define VTTBR_BADDR_MASK                                0x000000FFFFFFF000ULL
#define VTTBR_BADDR_SHIFT                               12
        
/* VTCR */
#define VTCR_INITVAL                                    0x80000000
#define VTCR_SH0_MASK                                   0x00003000
#define VTCR_SH0_SHIFT                                  12
#define VTCR_ORGN0_MASK                                 0x00000C00
#define VTCR_ORGN0_SHIFT                                10
#define VTCR_IRGN0_MASK                                 0x00000300
#define VTCR_IRGN0_SHIFT                                8
#define VTCR_SL0_MASK                                   0x000000C0
#define VTCR_SL0_SHIFT                                  6
#define VTCR_S_MASK                                     0x00000010
#define VTCR_S_SHIFT                                    4
#define VTCR_T0SZ_MASK                                  0x00000003
#define VTCR_T0SZ_SHIFT                                 0

/* Stage 2 Level 2 */
#define LPAE_S2L2_SHIFT		9
#define LPAE_S2L2_ENTRIES	(1 << LPAE_S2L2_SHIFT)
#define MMU_NUM_PAGETABLE_ENTRIES		64

static lpaed_t _hyp_pgtables[MMU_NUM_PAGETABLE_ENTRIES];

/* Statically allocated for now */
static lpaed_t _vttbr_pte_guest0[LPAE_S2L2_ENTRIES] __attribute((__aligned__(4096)));
static lpaed_t _vttbr_pte_guest1[LPAE_S2L2_ENTRIES] __attribute((__aligned__(4096)));
static lpaed_t *_vmid_ttbl[NUM_GUESTS_STATIC];

lpaed_t *vmm_vmid_ttbl(vmid_t vmid)
{
#warning "_vttbr_pte_guest0/1[] are not initialized"
	lpaed_t *ttbl = 0;
	if ( vmid < NUM_GUESTS_STATIC ) {
		ttbl = _vmid_ttbl[vmid];
	}
	return ttbl;
}

void vmm_stage2_enable(int enable)
{
	uint32_t hcr;

	// HCR.VM[0] = enable
	hcr = read_hcr(); //uart_print( "hcr:"); uart_print_hex32(hcr); uart_print("\n\r");
	if ( enable ) {
		hcr |= (0x1);
	} else {
		hcr &= ~(0x1);
	}
	write_hcr( hcr );
}

vmm_status_t vmm_set_vmid_ttbl( vmid_t vmid, lpaed_t *ttbl )
{
	// VTTBR.VMID = vmid
	// VTTBR.BADDR = ttbl
	uint64_t vttbr;

	vttbr = read_vttbr(); uart_print( "current vttbr:" ); uart_print_hex64(vttbr); uart_print("\n\r");
	vttbr &= ~(VTTBR_VMID_MASK);
	vttbr |= ((uint64_t)vmid << VTTBR_VMID_SHIFT) & VTTBR_VMID_MASK;

	vttbr &= ~(VTTBR_BADDR_MASK);
	vttbr |= (uint32_t) ttbl & VTTBR_BADDR_MASK;
	write_vttbr(vttbr);
	vttbr = read_vttbr(); uart_print( "changed vttbr:" ); uart_print_hex64(vttbr); uart_print("\n\r");
	return VMM_STATUS_SUCCESS;
}

void _vmm_init(void)
{
	int i;
	for( i = 0; i < NUM_GUESTS_STATIC; i++ ) {
		_vmid_ttbl[i] = 0;
	}

	_vmid_ttbl[0] = &_vttbr_pte_guest0[0];
	_vmid_ttbl[1] = &_vttbr_pte_guest0[1];
}

int mmu_init(void)
{
/*
 *	MAIR0, MAIR1
 *	HMAIR0, HMAIR1
 *	HTCR
 *	HTCTLR
 *	HTTBR
 * 	HTCTLR
 */
	uint32_t mair, htcr, hsctlr, vtcr, hcr;
	uint64_t httbr, vttbr;
	uart_print( "[mmu] mmu_init: enter\n\r" );
	
	_vmm_init();

	// MAIR/HMAIR
	uart_print(" --- MAIR ----\n\r" );
	mair = read_mair0(); uart_print( "mair0:"); uart_print_hex32(mair); uart_print("\n\r");
	mair = read_mair1(); uart_print( "mair1:"); uart_print_hex32(mair); uart_print("\n\r");
	mair = read_hmair0(); uart_print( "hmair0:"); uart_print_hex32(mair); uart_print("\n\r");
	mair = read_hmair1(); uart_print( "hmair1:"); uart_print_hex32(mair); uart_print("\n\r");

	write_mair0( INITIAL_MAIR0VAL );
	write_mair1( INITIAL_MAIR1VAL );
	write_hmair0( INITIAL_MAIR0VAL );
	write_hmair1( INITIAL_MAIR1VAL );

	mair = read_mair0(); uart_print( "mair0:"); uart_print_hex32(mair); uart_print("\n\r");
	mair = read_mair1(); uart_print( "mair1:"); uart_print_hex32(mair); uart_print("\n\r");
	mair = read_hmair0(); uart_print( "hmair0:"); uart_print_hex32(mair); uart_print("\n\r");
	mair = read_hmair1(); uart_print( "hmair1:"); uart_print_hex32(mair); uart_print("\n\r");

	// HTCR
	uart_print(" --- HTCR ----\n\r" );
	htcr = read_htcr(); uart_print( "htcr:"); uart_print_hex32(htcr); uart_print("\n\r");
	write_htcr( 0x80002500 );
	htcr = read_htcr(); uart_print( "htcr:"); uart_print_hex32(htcr); uart_print("\n\r");

	// HSCTLR
	// i-Cache and Alignment Checking Enabled
	// MMU, D-cache, Write-implies-XN, Low-latency IRQs Disabled
	hsctlr = read_hsctlr(); uart_print( "hsctlr:"); uart_print_hex32(hsctlr); uart_print("\n\r");
	hsctlr = HSCTLR_BASE | SCTLR_A;
	write_hsctlr( hsctlr );
	hsctlr = read_hsctlr(); uart_print( "hsctlr:"); uart_print_hex32(hsctlr); uart_print("\n\r");
	
	// HTTBR = &_hyp_pgtables
	httbr = read_httbr(); uart_print( "httbr:" ); uart_print_hex64(httbr); uart_print("\n\r");
	httbr &= 0xFFFFFFFF00000000ULL;
	httbr |= (uint32_t) &_hyp_pgtables;
	httbr &= HTTBR_BADDR_MASK;
	uart_print( "writing httbr:" ); uart_print_hex64(httbr); uart_print("\n\r");
	write_httbr( httbr );
	httbr = read_httbr(); uart_print( "read back httbr:" ); uart_print_hex64(httbr); uart_print("\n\r");

// TODO: Write PTE to _hyp_pgtables
#if 0
	// HSCTLR Enable MMU and D-cache
	hsctlr = read_hsctlr(); uart_print( "hsctlr:"); uart_print_hex32(hsctlr); uart_print("\n\r");
	hsctlr |= (SCTLR_M |SCTLR_C);
	
	// Flush PTE writes
	asm("dsb");
	write_hsctlr( hsctlr );
	// Flush iCache
	asm("isb");
	hsctlr = read_hsctlr(); uart_print( "hsctlr:"); uart_print_hex32(hsctlr); uart_print("\n\r");
#endif

// VTCR
	vtcr = read_vtcr(); uart_print( "vtcr:"); uart_print_hex32(vtcr); uart_print("\n\r");
	// start lookup at level 2 table
	vtcr &= ~VTCR_SL0_MASK;
	vtcr |= (0x0 << VTCR_SL0_SHIFT) & VTCR_SL0_MASK;
	vtcr &= ~VTCR_ORGN0_MASK;
	vtcr |= (0x3 << VTCR_ORGN0_SHIFT) & VTCR_ORGN0_MASK;
	vtcr &= ~VTCR_IRGN0_MASK;
	vtcr |= (0x3 << VTCR_IRGN0_SHIFT) & VTCR_IRGN0_MASK;
	write_vtcr(vtcr);
	vtcr = read_vtcr(); uart_print( "vtcr:"); uart_print_hex32(vtcr); uart_print("\n\r");
	{
		uint32_t sl0 = (vtcr & VTCR_SL0_MASK) >> VTCR_SL0_SHIFT;
		uint32_t t0sz = vtcr & 0xF;
		uint32_t baddr_x = (sl0 == 0 ? 14 - t0sz : 5 - t0sz);
		uart_print( "vttbr.baddr.x:"); uart_print_hex32(baddr_x); uart_print("\n\r");
	}
// VTTBR
	vttbr = read_vttbr(); uart_print( "vttbr:" ); uart_print_hex64(vttbr); uart_print("\n\r");

// HCR
	hcr = read_hcr(); uart_print( "hcr:"); uart_print_hex32(hcr); uart_print("\n\r");

	uart_print( "[mmu] mmu_init: exit\n\r" );
	
}
