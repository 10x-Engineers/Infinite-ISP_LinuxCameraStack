// SPDX-License-Identifier: GPL-2.0
/*
 * Infinite-ISP shared interrupt dispatcher.
 *
 * The FPGA combines the ISP, VIP1 and VIP2 interrupt outputs into one level
 * interrupt. This driver owns that parent IRQ once and fans it out to the
 * three register-bank handlers. Each child callback is responsible for
 * reading and acknowledging its own status register.
 */
#include <linux/bitops.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#include "xil-isp-irq.h"

struct xil_isp_irq_slot {
	xil_isp_irq_callback_t callback;
	void *data;
};

struct xil_isp_irq_dispatcher {
	struct mutex lock;
	spinlock_t slots_lock;
	struct xil_isp_irq_slot slots[XIL_ISP_IRQ_SOURCE_COUNT];
	struct device *owner;
	unsigned long enabled_sources;
	int irq;
};

static struct xil_isp_irq_dispatcher dispatcher = {
	.lock = __MUTEX_INITIALIZER(dispatcher.lock),
	.slots_lock = __SPIN_LOCK_UNLOCKED(dispatcher.slots_lock),
	.irq = -1,
};

static irqreturn_t xil_isp_irq_thread(int irq, void *data)
{
	struct xil_isp_irq_slot slots[XIL_ISP_IRQ_SOURCE_COUNT];
	irqreturn_t handled = IRQ_NONE;
	unsigned long flags;
	unsigned int i;

	spin_lock_irqsave(&dispatcher.slots_lock, flags);
	memcpy(slots, dispatcher.slots, sizeof(slots));
	spin_unlock_irqrestore(&dispatcher.slots_lock, flags);

	/*
	 * Always poll every registered bank. A level interrupt remains asserted
	 * until every contributing block has acknowledged its local status.
	 */
	for (i = 0; i < XIL_ISP_IRQ_SOURCE_COUNT; i++) {
		if (slots[i].callback && slots[i].callback(slots[i].data) == IRQ_HANDLED)
			handled = IRQ_HANDLED;
	}

	return handled;
}

int xil_isp_irq_attach(struct device *dev, int irq)
{
	int ret = 0;

	if (!dev || irq < 0)
		return -EINVAL;

	mutex_lock(&dispatcher.lock);
	if (dispatcher.owner) {
		ret = dispatcher.owner == dev && dispatcher.irq == irq ? 0 : -EBUSY;
		goto out;
	}

	ret = request_threaded_irq(irq, NULL, xil_isp_irq_thread,
				   IRQF_ONESHOT | IRQF_NO_AUTOEN,
				   "xil-isp-irq-dispatcher", &dispatcher);
	if (ret)
		goto out;

	dispatcher.owner = dev;
	dispatcher.irq = irq;
	dispatcher.enabled_sources = 0;
	dev_info(dev, "shared ISP/VIP IRQ dispatcher attached to IRQ %d", irq);
out:
	mutex_unlock(&dispatcher.lock);
	return ret;
}
EXPORT_SYMBOL_GPL(xil_isp_irq_attach);

void xil_isp_irq_detach(struct device *dev)
{
	int irq = -1;

	mutex_lock(&dispatcher.lock);
	if (dispatcher.owner == dev) {
		irq = dispatcher.irq;
		if (dispatcher.enabled_sources)
			disable_irq(irq);
		dispatcher.enabled_sources = 0;
		dispatcher.owner = NULL;
		dispatcher.irq = -1;
	}
	mutex_unlock(&dispatcher.lock);

	if (irq >= 0)
		free_irq(irq, &dispatcher);
}
EXPORT_SYMBOL_GPL(xil_isp_irq_detach);

int xil_isp_irq_register(enum xil_isp_irq_source source,
			 xil_isp_irq_callback_t callback, void *data)
{
	unsigned long flags;
	int ret = 0;

	if (source >= XIL_ISP_IRQ_SOURCE_COUNT || !callback)
		return -EINVAL;

	spin_lock_irqsave(&dispatcher.slots_lock, flags);
	if (dispatcher.slots[source].callback &&
	    (dispatcher.slots[source].callback != callback ||
	     dispatcher.slots[source].data != data)) {
		ret = -EBUSY;
	} else {
		dispatcher.slots[source].callback = callback;
		dispatcher.slots[source].data = data;
	}
	spin_unlock_irqrestore(&dispatcher.slots_lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(xil_isp_irq_register);

void xil_isp_irq_unregister(enum xil_isp_irq_source source, void *data)
{
	unsigned long flags;
	int irq;

	if (source >= XIL_ISP_IRQ_SOURCE_COUNT)
		return;

	spin_lock_irqsave(&dispatcher.slots_lock, flags);
	if (dispatcher.slots[source].data == data) {
		dispatcher.slots[source].callback = NULL;
		dispatcher.slots[source].data = NULL;
	}
	spin_unlock_irqrestore(&dispatcher.slots_lock, flags);

	mutex_lock(&dispatcher.lock);
	irq = dispatcher.irq;
	mutex_unlock(&dispatcher.lock);
	if (irq >= 0)
		synchronize_irq(irq);
}
EXPORT_SYMBOL_GPL(xil_isp_irq_unregister);

int xil_isp_irq_enable(enum xil_isp_irq_source source)
{
	unsigned long bit;
	int ret = 0;

	if (source >= XIL_ISP_IRQ_SOURCE_COUNT)
		return -EINVAL;
	bit = BIT(source);

	mutex_lock(&dispatcher.lock);
	if (!dispatcher.owner) {
		ret = -ENODEV;
		goto out;
	}

	if (!(dispatcher.enabled_sources & bit)) {
		if (!dispatcher.enabled_sources)
			enable_irq(dispatcher.irq);
		dispatcher.enabled_sources |= bit;
	}
out:
	mutex_unlock(&dispatcher.lock);
	return ret;
}
EXPORT_SYMBOL_GPL(xil_isp_irq_enable);

void xil_isp_irq_disable(enum xil_isp_irq_source source)
{
	unsigned long bit;

	if (source >= XIL_ISP_IRQ_SOURCE_COUNT)
		return;
	bit = BIT(source);

	mutex_lock(&dispatcher.lock);
	if (dispatcher.owner && (dispatcher.enabled_sources & bit)) {
		dispatcher.enabled_sources &= ~bit;
		if (!dispatcher.enabled_sources)
			disable_irq(dispatcher.irq);
	}
	mutex_unlock(&dispatcher.lock);
}
EXPORT_SYMBOL_GPL(xil_isp_irq_disable);

static int xil_isp_irq_probe(struct platform_device *pdev)
{
	int irq = platform_get_irq(pdev, 0);

	if (irq < 0)
		return irq;

	return xil_isp_irq_attach(&pdev->dev, irq);
}

static int xil_isp_irq_remove(struct platform_device *pdev)
{
	xil_isp_irq_detach(&pdev->dev);
	return 0;
}

static const struct of_device_id xil_isp_irq_of_match[] = {
	{ .compatible = "xlnx,infinite-isp-irq-dispatcher" },
	{ }
};
MODULE_DEVICE_TABLE(of, xil_isp_irq_of_match);

static struct platform_driver xil_isp_irq_driver = {
	.probe = xil_isp_irq_probe,
	.remove = xil_isp_irq_remove,
	.driver = {
		.name = "xil-isp-irq-dispatcher",
		.of_match_table = xil_isp_irq_of_match,
	},
};
module_platform_driver(xil_isp_irq_driver);

MODULE_AUTHOR("Infinite-ISP contributors");
MODULE_DESCRIPTION("Infinite-ISP shared ISP/VIP IRQ dispatcher");
MODULE_LICENSE("GPL v2");
