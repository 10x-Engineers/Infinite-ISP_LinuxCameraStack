/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __XIL_ISP_IRQ_H__
#define __XIL_ISP_IRQ_H__

#include <linux/device.h>
#include <linux/interrupt.h>

/* One physical FPGA IRQ is shared by these three register banks. */
enum xil_isp_irq_source {
	XIL_ISP_IRQ_SOURCE_ISP = 0,
	XIL_ISP_IRQ_SOURCE_VIP1,
	XIL_ISP_IRQ_SOURCE_VIP2,
	XIL_ISP_IRQ_SOURCE_COUNT,
};

typedef irqreturn_t (*xil_isp_irq_callback_t)(void *data);

/*
 * Legacy attachment lets the existing ISP DT node donate its parent IRQ.
 * A dedicated xlnx,infinite-isp-irq-dispatcher DT node calls the same API.
 */
int xil_isp_irq_attach(struct device *dev, int irq);
void xil_isp_irq_detach(struct device *dev);

int xil_isp_irq_register(enum xil_isp_irq_source source,
			 xil_isp_irq_callback_t callback, void *data);
void xil_isp_irq_unregister(enum xil_isp_irq_source source, void *data);

int xil_isp_irq_enable(enum xil_isp_irq_source source);
void xil_isp_irq_disable(enum xil_isp_irq_source source);

#endif /* __XIL_ISP_IRQ_H__ */
