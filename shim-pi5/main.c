/*
 * Pi 5 bring-up, stage 1: one boot answers the questions the XNU port
 * depends on, on the UART and on HDMI.
 *   - did we reach EL1 from the firmware's EL2
 *   - CPU features: 16K granule, PAC, GIC sysreg interface
 *   - device tree: memory, UART, GIC-400, timer PPIs, simple-framebuffer, PSCI
 *   - GICv2: does the virtual timer PPI arrive as an IRQ at EL1
 *   - SMP: PSCI CPU_ON for cpu1-3, each reports MPIDR and entry EL
 * Nothing is hardcoded that the DT can tell us; fallbacks are bcm2712.dtsi values.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"

#define UART10_FALLBACK		0x107d001000UL	/* bcm2712 debug UART (JST header) */
#define NR_CPUS			4
#define QEMU_FAKE_FB		0x7f400000UL	/* QEMU virt RAM; same 32MB-block offset (0x1400000) as the Pi 5 fb */

/* GICv2 (GIC-400) */
#define GICD_CTLR		0x000
#define GICD_TYPER		0x004
#define GICD_IIDR		0x008
#define GICD_ISENABLER		0x100
#define GICD_ICENABLER		0x180
#define GICD_IPRIORITYR		0x400
#define GICC_CTLR		0x00
#define GICC_PMR		0x04
#define GICC_BPR		0x08
#define GICC_IAR		0x0c
#define GICC_EOIR		0x10
#define GICC_IIDR		0xfc

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_FEATURES		0x8400000aUL
#define SMCCC_VERSION		0x80000000UL

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")
#define dsb() __asm__ volatile("dsb sy" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];

static uintptr_t gicd, gicc;
static int psci_hvc;
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile uint32_t timer_intid;
static volatile uint64_t sec_mpidr[NR_CPUS];
static volatile uint32_t sec_el[NR_CPUS];
static int failures;
static struct loader_video video;

static uint64_t psci(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	if (psci_hvc)
		__asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :
				 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
				   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	else
		__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :
				 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
				   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

static void check(const char *what, int ok, const char *detail)
{
	con_color(ok ? C_GREEN : C_RED);
	pr("[%s] ", ok ? " OK " : "FAIL");
	con_color(C_WHITE);
	pr("%s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
	if (!ok)
		failures++;
}

/* ---- exceptions ---- */

static void irq(void)
{
	uint32_t iar = rd32(gicc + GICC_IAR);
	uint32_t id = iar & 0x3ff;
	if (id >= 1020)
		return;	/* spurious */
	irq_last = id;
	if (id == timer_intid) {
		irq_count++;
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
	} else {
		irq_other++;
	}
	wr32(gicc + GICC_EOIR, iar);
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	(void)regs;
	if ((slot == 5 || slot == 1) && gicc) {	/* IRQ from EL1 SPx / SP0 */
		irq();
		return;
	}
	fb_clear(C_PANIC);
	con_color(C_WHITE);
	pr("PI5 SHIM EXCEPTION slot %u on cpu mpidr %lx\n", (unsigned)slot, rsr("mpidr_el1"));
	pr("ESR %lx  ELR %lx  FAR %lx\n", rsr("esr_el1"), rsr("elr_el1"), rsr("far_el1"));
	for (;;)
		__asm__ volatile("wfe");
}

void secondary_main(uint64_t idx, uint64_t el)
{
	if (idx < NR_CPUS) {
		sec_el[idx] = (uint32_t)el;
		sec_mpidr[idx] = rsr("mpidr_el1") | 1UL << 63;	/* bit 63 = arrived */
		dsb();
	}
}

/* ---- device tree ---- */

static const char *dt_string(uintptr_t fdt, const struct fdt_node *n, const char *prop)
{
	uintptr_t v;
	uint32_t l;
	if (!fdt_prop(fdt, n, prop, &v, &l) || !l)
		return 0;
	return (const char *)v;
}

/* /chosen stdout-path ("serial10:115200n8" or a path), else alias serial10. */
static int find_uart(uintptr_t fdt, uintptr_t *base, uint32_t *clk)
{
	struct fdt_node n, a;
	const char *path = 0;
	char buf[96];

	if (fdt_find_path(fdt, "/chosen", &n))
		path = dt_string(fdt, &n, "stdout-path");
	if (!path) {
		if (!fdt_find_path(fdt, "/aliases", &a))
			return 0;
		path = dt_string(fdt, &a, "serial10");
		if (!path)
			return 0;
	}
	/* copy, cut at ':' */
	unsigned i = 0;
	while (i < sizeof(buf) - 1 && path[i] && path[i] != ':')
		buf[i] = path[i], i++;
	buf[i] = 0;
	if (buf[0] != '/') {	/* alias name */
		if (!fdt_find_path(fdt, "/aliases", &a))
			return 0;
		path = dt_string(fdt, &a, buf);
		if (!path)
			return 0;
		for (i = 0; i < sizeof(buf) - 1 && path[i]; i++)
			buf[i] = path[i];
		buf[i] = 0;
	}
	if (!fdt_find_path(fdt, buf, &n))
		return 0;
	uint64_t addr, size;
	if (!fdt_reg(fdt, &n, 0, &addr, &size))
		return 0;
	*base = addr;
	*clk = 0;
	uint32_t ph;
	if (fdt_prop_u32(fdt, &n, "clocks", &ph)) {
		struct fdt_node c;
		if (fdt_find_phandle(fdt, ph, &c))
			fdt_prop_u32(fdt, &c, "clock-frequency", clk);
	}
	return 1;
}

static void setup_fb(uintptr_t fdt)
{
	struct fdt_node n;
	uint64_t addr, size;
	uint32_t w = 0, h = 0, stride = 0, bpp = 32;
	if (!fdt_find_compatible(fdt, "simple-framebuffer", 0, &n) || !fdt_reg(fdt, &n, 0, &addr, &size)) {
		/*
		 * QEMU virt has no framebuffer. Report a fake 1920x1080 one in spare
		 * guest RAM so xnu's early-console path (start.s VA=PA block, the
		 * final kernel mapping in arm_vm_init) runs exactly as on a Pi; dump
		 * it from the QEMU monitor (pmemsave) to see what was drawn.
		 */
		if (fdt_find_compatible(fdt, "linux,dummy-virt", 0, &n)) {
			video.base = QEMU_FAKE_FB;
			video.width = 1920;
			video.height = 1080;
			video.stride = 1920 * 4;
			pr("QEMU: fake framebuffer %lx 1920x1080 (RAM, see qemu.sh)\n", video.base);
		}
		return;
	}
	fdt_prop_u32(fdt, &n, "width", &w);
	fdt_prop_u32(fdt, &n, "height", &h);
	fdt_prop_u32(fdt, &n, "stride", &stride);
	const char *fmt = dt_string(fdt, &n, "format");
	if (fmt && fdt_streq(fmt, "r5g6b5"))
		bpp = 16;
	fb_init(addr, w, h, stride, bpp);
	fb_clear(C_BG);
	if (bpp == 32) {
		video.base = addr;
		video.width = w;
		video.height = h;
		video.stride = stride ? stride : w * 4;
	}
	pr("framebuffer %lx %ux%u stride %u %s\n", addr, w, h, stride, fmt ? fmt : "?");
}

static void report_memory(uintptr_t fdt)
{
	struct fdt_node n;
	uint64_t total = 0;
	for (int i = 0; fdt_find_name(fdt, "memory", 1, i, &n); i++) {
		uint64_t a, s;
		for (int r = 0; fdt_reg(fdt, &n, r, &a, &s); r++) {
			pr("  ram %011lx + %lx\n", a, s);
			total += s;
		}
	}
	pr("  total %lu MiB\n", total >> 20);
	check("RAM from DT", total >= (4UL << 30), total >= (7UL << 30) ? "8 GB model" : "less than 8 GB?");
}

static void report_cpu(uint64_t entry_el)
{
	uint64_t midr = rsr("midr_el1"), mmfr0 = rsr("id_aa64mmfr0_el1");
	uint64_t pfr0 = rsr("id_aa64pfr0_el1"), isar1 = rsr("id_aa64isar1_el1");
	pr("MIDR %lx  MPIDR %lx  CNTFRQ %lu\n", midr, rsr("mpidr_el1"), rsr("cntfrq_el0"));
	pr("MMFR0 %lx  PFR0 %lx  ISAR1 %lx\n", mmfr0, pfr0, isar1);
	check("entered at EL2, now EL1", entry_el == 2 && (rsr("CurrentEL") >> 2) == 1,
	      entry_el == 1 ? "firmware entered at EL1" : 0);
	check("Cortex-A76", (midr & 0xff00fff0) == 0x4100d0b0, 0);
	/* TGran16 [23:20]: 1 = 16K supported (XNU arm64 page size) */
	check("16K granule", ((mmfr0 >> 20) & 0xf) >= 1, 0);
	/* PAC: APA [7:4] / API [11:8]: 0 means plain arm64 userland is required */
	check("no PAC (plain arm64)", ((isar1 >> 4) & 0xff) == 0, 0);
	/* GIC [27:24]: 0 = no sysreg CPU interface -> GICv2 MMIO path */
	check("GIC sysregs absent (GICv2 MMIO)", ((pfr0 >> 24) & 0xf) == 0, 0);
}

/* ---- GICv2 timer test ---- */

static void test_gic(uintptr_t fdt)
{
	struct fdt_node n;
	uint64_t d, c, s;
	/* Pi 5: "arm,gic-400". QEMU virt (gic-version=2): "arm,cortex-a15-gic". */
	int found = fdt_find_compatible(fdt, "arm,gic-400", 0, &n) ||
		    fdt_find_compatible(fdt, "arm,cortex-a15-gic", 0, &n);
	if (!found || !fdt_reg(fdt, &n, 0, &d, &s) || !fdt_reg(fdt, &n, 1, &c, &s)) {
		check("GIC-400 in DT", 0, 0);
		return;
	}
	gicd = d;
	gicc = c;
	pr("GICD %lx  GICC %lx\n", d, c);
	pr("GICD_TYPER %x  GICD_IIDR %x  GICC_IIDR %x\n",
	   rd32(gicd + GICD_TYPER), rd32(gicd + GICD_IIDR), rd32(gicc + GICC_IIDR));

	/* timer: interrupts = 4 x <type num flags>: sec-phys, phys, virt, hyp */
	uint32_t ppi[4] = { 13, 14, 11, 10 };
	if (fdt_find_compatible(fdt, "arm,armv8-timer", 0, &n)) {
		uintptr_t v;
		uint32_t l;
		if (fdt_prop(fdt, &n, "interrupts", &v, &l) && l >= 48)
			for (int i = 0; i < 4; i++)
				ppi[i] = fdt_be32(v + 12 * i + 4);
	}
	pr("timer PPIs: phys %u virt %u hyp %u (INTID = PPI + 16)\n", ppi[1], ppi[2], ppi[3]);
	timer_intid = ppi[2] + 16;

	uint32_t id = timer_intid;
	wr32(gicd + GICD_CTLR, rd32(gicd + GICD_CTLR) | 1);	/* NS view: EnableGrp1 */
	wr8(gicd + GICD_IPRIORITYR + id, 0xa0);
	wr32(gicd + GICD_ISENABLER + 4 * (id / 32), 1U << (id % 32));
	wr32(gicc + GICC_PMR, 0xf0);
	wr32(gicc + GICC_BPR, 0);
	wr32(gicc + GICC_CTLR, 1);
	dsb();

	wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);
	wsr("cntv_ctl_el0", 1);
	isb();
	__asm__ volatile("msr daifclr, #2" ::: "memory");
	delay_ms(300);
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	wr32(gicd + GICD_ICENABLER + 4 * (id / 32), 1U << (id % 32));
	isb();

	pr("vtimer IRQs in 300 ms: %u (expect ~30), other %u, last %u\n",
	   irq_count, irq_other, irq_last);
	check("GICv2 vtimer IRQ at EL1", irq_count >= 20 && irq_count <= 40, 0);
}

/* ---- SMP ---- */

static void test_smp(uintptr_t fdt)
{
	struct fdt_node n;
	if (fdt_find_path(fdt, "/psci", &n) || fdt_find_compatible(fdt, "arm,psci-1.0", 0, &n)) {
		const char *m = dt_string(fdt, &n, "method");
		psci_hvc = m && fdt_streq(m, "hvc");
	}
	uint64_t ver = psci(PSCI_VERSION, 0, 0, 0);
	pr("PSCI %u.%u via %s, SMCCC %lx\n", (unsigned)(ver >> 16), (unsigned)(ver & 0xffff),
	   psci_hvc ? "hvc" : "smc", psci(SMCCC_VERSION, 0, 0, 0));

	uint64_t self = rsr("mpidr_el1") & 0xff00ffffffUL;
	int idx = 1, started = 0, want = 0;
	for (int i = 0; fdt_find_compatible(fdt, "arm,cortex-a76", i, &n); i++) {
		uint64_t mpidr;
		/* /cpus has #size-cells = 0, so reg is one address */
		uintptr_t v;
		uint32_t l;
		if (!fdt_prop(fdt, &n, "reg", &v, &l) || !l)
			continue;
		mpidr = fdt_cells(v, l / 4 > 2 ? 2 : l / 4);
		if (mpidr == self || idx >= NR_CPUS)
			continue;
		want++;
		uint64_t r = psci(PSCI_CPU_ON, mpidr, (uintptr_t)secondary_entry, idx);
		uint64_t end = now() + rsr("cntfrq_el0") / 2;
		while (!(sec_mpidr[idx] >> 63) && now() < end)
			;
		pr("  cpu%d mpidr %lx: CPU_ON %ld, ", idx, mpidr, (int64_t)r);
		if (sec_mpidr[idx] >> 63) {
			pr("up, entered EL%u, mpidr %lx\n", sec_el[idx], sec_mpidr[idx] & ~(1UL << 63));
			started++;
		} else {
			pr("no response\n");
		}
		idx++;
	}
	check("secondaries via PSCI CPU_ON", want == 3 && started == 3, want == 3 ? 0 : "expected 3 cpu nodes");
}

void shim_main(uintptr_t fdt, uint64_t entry_el)
{
	uintptr_t ub = UART10_FALLBACK;
	uint32_t clk = 0;
	int dt_ok = fdt_valid(fdt);
	int uart_from_dt = dt_ok && find_uart(fdt, &ub, &clk);
	uart_init(ub, clk, 115200);

	pr("\n\n==== ios-pi5 shim, stage 1 ====\n");
	if (dt_ok)
		setup_fb(fdt);
	con_color(C_YELLOW);
	pr("ios-pi5 shim - stage 1 bring-up\n");
	con_color(C_WHITE);
	pr("DTB at %lx, UART %lx (%s)\n", fdt, ub, uart_from_dt ? "from DT" : "fallback address");
	check("device tree", dt_ok, 0);

	report_cpu(entry_el);
	uint64_t rd_start, rd_end;
	if (dt_ok && loader_find(fdt, &rd_start, &rd_end)) {
		/*
		 * Stage 2: an xnu payload is loaded. Skip the GIC and SMP tests:
		 * xnu programs the GIC itself and starts cpu1-3 with PSCI CPU_ON,
		 * which fails for a core this shim already started.
		 */
		con_color(C_YELLOW);
		pr("\nSWEETXNU payload found: booting xnu\n");
		con_color(C_WHITE);
		const char *err = loader_boot(rd_start, rd_end, &video);
		con_color(C_RED);
		pr("BOOT FAILED: %s\n", err);
		con_color(C_WHITE);
		return;
	}
	if (dt_ok) {
		report_memory(fdt);
		test_gic(fdt);
		test_smp(fdt);
	}

	con_color(failures ? C_RED : C_GREEN);
	pr("\n%s: %d check%s failed\n", failures ? "STAGE 1 INCOMPLETE" : "STAGE 1 PASSED",
	   failures, failures == 1 ? "" : "s");
	con_color(C_WHITE);
	pr("Parked. Power off to stop.\n");
}
