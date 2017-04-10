/*
 *	Macintosh interrupts
 *
 * General design:
 * In contrary to the Amiga and Atari platforms, the Mac hardware seems to
 * exclusively use the autovector interrupts (the 'generic level0-level7'
 * interrupts with exception vectors 0x19-0x1f). The following interrupt levels
 * are used:
 *	1	- VIA1
 *		  - slot 0: one second interrupt (CA2)
 *		  - slot 1: VBlank (CA1)
 *		  - slot 2: ADB data ready (SR full)
 *		  - slot 3: ADB data  (CB2)
 *		  - slot 4: ADB clock (CB1)
 *		  - slot 5: timer 2
 *		  - slot 6: timer 1
 *		  - slot 7: status of IRQ; signals 'any enabled int.'
 *
 *	2	- VIA2 or RBV
 *		  - slot 0: SCSI DRQ (CA2)
 *		  - slot 1: NUBUS IRQ (CA1) need to read port A to find which
 *		  - slot 2: /EXP IRQ (only on IIci)
 *		  - slot 3: SCSI IRQ (CB2)
 *		  - slot 4: ASC IRQ (CB1)
 *		  - slot 5: timer 2 (not on IIci)
 *		  - slot 6: timer 1 (not on IIci)
 *		  - slot 7: status of IRQ; signals 'any enabled int.'
 *
 * Levels 3-6 vary by machine type. For VIA or RBV Macintoshes:
 *
 *	3	- unused (?)
 *
 *	4	- SCC
 *
 *	5	- unused (?)
 *		  [serial errors or special conditions seem to raise level 6
 *		  interrupts on some models (LC4xx?)]
 *
 *	6	- off switch (?)
 *
 * Machines with Quadra-like VIA hardware, except PSC and PMU machines, support
 * an alternate interrupt mapping, as used by A/UX. It spreads ethernet and
 * sound out to their own autovector IRQs and gives VIA1 a higher priority:
 *
 *	1	- unused (?)
 *
 *	3	- on-board SONIC
 *
 *	5	- Apple Sound Chip (ASC)
 *
 *	6	- VIA1
 *
 * For OSS Macintoshes (IIfx only), we apply an interrupt mapping similar to
 * the Quadra (A/UX) mapping:
 *
 *	1	- ISM IOP (ADB)
 *
 *	2	- SCSI
 *
 *	3	- NuBus
 *
 *	4	- SCC IOP
 *
 *	6	- VIA1
 *
 * For PSC Macintoshes (660AV, 840AV):
 *
 *	3	- PSC level 3
 *		  - slot 0: MACE
 *
 *	4	- PSC level 4
 *		  - slot 1: SCC channel A interrupt
 *		  - slot 2: SCC channel B interrupt
 *		  - slot 3: MACE DMA
 *
 *	5	- PSC level 5
 *
 *	6	- PSC level 6
 *
 * Finally we have good 'ole level 7, the non-maskable interrupt:
 *
 *	7	- NMI (programmer's switch on the back of some Macs)
 *		  Also RAM parity error on models which support it (IIc, IIfx?)
 *
 * The current interrupt logic looks something like this:
 *
 * - We install dispatchers for the autovector interrupts (1-7). These
 *   dispatchers are responsible for querying the hardware (the
 *   VIA/RBV/OSS/PSC chips) to determine the actual interrupt source. Using
 *   this information a machspec interrupt number is generated by placing the
 *   index of the interrupt hardware into the low three bits and the original
 *   autovector interrupt number in the upper 5 bits. The handlers for the
 *   resulting machspec interrupt are then called.
 *
 * - Nubus is a special case because its interrupts are hidden behind two
 *   layers of hardware. Nubus interrupts come in as index 1 on VIA #2,
 *   which translates to IRQ number 17. In this spot we install _another_
 *   dispatcher. This dispatcher finds the interrupting slot number (9-F) and
 *   then forms a new machspec interrupt number as above with the slot number
 *   minus 9 in the low three bits and the pseudo-level 7 in the upper five
 *   bits.  The handlers for this new machspec interrupt number are then
 *   called. This puts Nubus interrupts into the range 56-62.
 *
 * - The Baboon interrupts (used on some PowerBooks) are an even more special
 *   case. They're hidden behind the Nubus slot $C interrupt thus adding a
 *   third layer of indirection. Why oh why did the Apple engineers do that?
 *
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/delay.h>

#include <asm/irq.h>
#include <asm/macintosh.h>
#include <asm/macints.h>
#include <asm/mac_via.h>
#include <asm/mac_psc.h>
#include <asm/mac_oss.h>
#include <asm/mac_iop.h>
#include <asm/mac_baboon.h>
#include <asm/hwtest.h>
#include <asm/irq_regs.h>

extern void show_registers(struct pt_regs *);

irqreturn_t mac_nmi_handler(int, void *);

static unsigned int mac_irq_startup(struct irq_data *);
static void mac_irq_shutdown(struct irq_data *);

static struct irq_chip mac_irq_chip = {
	.name		= "mac",
	.irq_enable	= mac_irq_enable,
	.irq_disable	= mac_irq_disable,
	.irq_startup	= mac_irq_startup,
	.irq_shutdown	= mac_irq_shutdown,
};

void __init mac_init_IRQ(void)
{
	m68k_setup_irq_controller(&mac_irq_chip, handle_simple_irq, IRQ_USER,
				  NUM_MAC_SOURCES - IRQ_USER);

	/*
	 * Now register the handlers for the master IRQ handlers
	 * at levels 1-7. Most of the work is done elsewhere.
	 */

	if (oss_present)
		oss_register_interrupts();
	else
		via_register_interrupts();
	if (psc)
		psc_register_interrupts();
	if (baboon_present)
		baboon_register_interrupts();
	iop_register_interrupts();
	if (request_irq(IRQ_AUTO_7, mac_nmi_handler, 0, "NMI",
			mac_nmi_handler))
		pr_err("Couldn't register NMI\n");
}

/*
 *  mac_irq_enable - enable an interrupt source
 * mac_irq_disable - disable an interrupt source
 *
 * These routines are just dispatchers to the VIA/OSS/PSC routines.
 */

void mac_irq_enable(struct irq_data *data)
{
	int irq = data->irq;
	int irq_src = IRQ_SRC(irq);

	switch(irq_src) {
	case 1:
	case 2:
	case 7:
		if (oss_present)
			oss_irq_enable(irq);
		else
			via_irq_enable(irq);
		break;
	case 3:
	case 4:
	case 5:
	case 6:
		if (psc)
			psc_irq_enable(irq);
		else if (oss_present)
			oss_irq_enable(irq);
		break;
	case 8:
		if (baboon_present)
			baboon_irq_enable(irq);
		break;
	}
}

void mac_irq_disable(struct irq_data *data)
{
	int irq = data->irq;
	int irq_src = IRQ_SRC(irq);

	switch(irq_src) {
	case 1:
	case 2:
	case 7:
		if (oss_present)
			oss_irq_disable(irq);
		else
			via_irq_disable(irq);
		break;
	case 3:
	case 4:
	case 5:
	case 6:
		if (psc)
			psc_irq_disable(irq);
		else if (oss_present)
			oss_irq_disable(irq);
		break;
	case 8:
		if (baboon_present)
			baboon_irq_disable(irq);
		break;
	}
}

static unsigned int mac_irq_startup(struct irq_data *data)
{
	int irq = data->irq;

	if (IRQ_SRC(irq) == 7 && !oss_present)
		via_nubus_irq_startup(irq);
	else
		mac_irq_enable(data);

	return 0;
}

static void mac_irq_shutdown(struct irq_data *data)
{
	int irq = data->irq;

	if (IRQ_SRC(irq) == 7 && !oss_present)
		via_nubus_irq_shutdown(irq);
	else
		mac_irq_disable(data);
}

static volatile int in_nmi;

irqreturn_t mac_nmi_handler(int irq, void *dev_id)
{
	if (in_nmi)
		return IRQ_HANDLED;
	in_nmi = 1;

	pr_info("Non-Maskable Interrupt\n");
	show_registers(get_irq_regs());

	in_nmi = 0;
	return IRQ_HANDLED;
}
