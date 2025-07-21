// SPDX-License-Identifier: GPL-2.0
/*
 *  Microchip COREUART driver.
 *
 *  Copyright (c) 2021-2025 Microchip Corporation. All rights reserved.
 *
 *  Author: Prashanth Kumar Burujukindi <prashanthkumar.burujukindi@microchip.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/serial_core.h>
#include <linux/serial_reg.h>
#include <linux/slab.h>
#include <linux/tty_flip.h>

#define MCHP_COREUART_TXDATA			0x00
#define MCHP_COREUART_RXDATA			0x04
#define MCHP_COREUART_CTRL1			0x08
#define MCHP_COREUART_CTRL2			0x0C
#define MCHP_COREUART_STATUS			0x10
#define MCHP_COREUART_CTRL3			0x14

#define MCHP_COREUART_STATUS_TXRDY_MASK		0x01
#define MCHP_COREUART_STATUS_RXRDY_MASK		0x02
#define MCHP_COREUART_STATUS_PARERR_MASK	0x04
#define MCHP_COREUART_STATUS_OVRFLW_MASK	0x08
#define MCHP_COREUART_STATUS_FRMERR_MASK	0x10

#define MCHP_COREUART_BAUD_VALUE_UPPER		GENMASK(4, 0)
#define MCHP_COREUART_BAUD_VALUE_LOWER		GENMASK(7, 0)
/* Sets a high limit for CoreUART ports, actual use driven by DT aliases from serial5 */
#define MCHP_COREUART_MAX_SERIAL_INSTANCE	16
#define MCHP_COREUART_MPFS_FIFO_SIZE		256
#define MCHP_COREUART_TXRDY_TIMEOUT		1000
#define MCHP_COREUART_TXRDY_WAIT_TIMEOUT	10000
static struct uart_driver mchp_core_uart_driver;

static struct uart_port ports[MCHP_COREUART_MAX_SERIAL_INSTANCE];

struct mchp_coreuart_port {
	struct uart_port port;
	struct clk *clk;
	void __iomem *base;
};

/* These definitions are required by uart_ops, CoreUART doesn't need explicit handling here. */
static void mchp_coreuart_stop_tx(struct uart_port *port) {}
static void mchp_coreuart_stop_rx(struct uart_port *port) {}
static void mchp_coreuart_set_mctrl(struct uart_port *port, unsigned int mctrl) {}
static unsigned int mchp_coreuart_get_mctrl(struct uart_port *port)
{
	return 0;
}
static int mchp_coreuart_request_port(struct uart_port *port)
{
	return 0;
}
static void mchp_coreuart_release_port(struct uart_port *port) {}

static unsigned int mchp_coreuart_tx_empty(struct uart_port *port)
{
	return readb(port->membase + MCHP_COREUART_STATUS)
		& MCHP_COREUART_STATUS_TXRDY_MASK;
}

static void mchp_coreuart_start_tx(struct uart_port *port)
{
	struct tty_port *tport = &port->state->port;

	while (!kfifo_is_empty(&tport->xmit_fifo)) {
		int timeout = MCHP_COREUART_TXRDY_TIMEOUT;
		int ret;
		u8 c;

		while (timeout--) {
			if (readb(port->membase + MCHP_COREUART_STATUS)
			    & MCHP_COREUART_STATUS_TXRDY_MASK)
				break;
		}

		if (timeout <= 0) {
			dev_warn(port->dev, "Timeout waiting for TX ready\n");
			break;
		}

		ret = kfifo_out(&tport->xmit_fifo, (u8 *)&c, sizeof(c));
		WARN_ON(!ret);
		writeb(c, port->membase + MCHP_COREUART_TXDATA);
		port->icount.tx++;
	}
}


static irqreturn_t mchp_coreuart_interrupt(int irq, void *dev_id)
{
	struct uart_port *port = (struct uart_port *)dev_id;
	struct tty_port *tport = &port->state->port;
	unsigned char received_data;
	unsigned int status;

	uart_port_lock(port);
	status = readb(port->membase + MCHP_COREUART_STATUS);

	if (!(status & MCHP_COREUART_STATUS_RXRDY_MASK))
		goto done;

	while ((readb(port->membase + MCHP_COREUART_STATUS)
		& MCHP_COREUART_STATUS_RXRDY_MASK)) {
		status = readb(port->membase + MCHP_COREUART_STATUS);
		received_data = readb(port->membase + MCHP_COREUART_RXDATA);
		port->icount.rx++;

		if (status & MCHP_COREUART_STATUS_PARERR_MASK) {
			port->icount.parity++;
			tty_insert_flip_char(tport, received_data, TTY_PARITY);
		}

		if (status & MCHP_COREUART_STATUS_OVRFLW_MASK) {
			port->icount.overrun++;
			tty_insert_flip_char(tport, received_data, TTY_OVERRUN);
		}

		if (status & MCHP_COREUART_STATUS_FRMERR_MASK) {
			port->icount.frame++;
			tty_insert_flip_char(tport, received_data, TTY_FRAME);
		}

		if (!tty_insert_flip_char(tport, received_data, TTY_NORMAL)) {
			dev_warn(port->dev, "TTY flip buffer overflow\n");
			break;
		}
	}

done:
	uart_unlock_and_check_sysrq(port);
	tty_flip_buffer_push(tport);
	return IRQ_HANDLED;
}

static void mchp_coreuart_set_termios(struct uart_port *port, struct ktermios *termios,
				      const struct ktermios *old)
{
	struct mchp_coreuart_port *apb_uart = container_of(port, struct mchp_coreuart_port, port);
	unsigned int baud = tty_termios_baud_rate(termios);
	unsigned int clk_rate =  apb_uart->port.uartclk;
	u16 baud_div = (clk_rate / (16 * baud)) - 1;
	u8 ctrl2 = 0;

	/* No of Data bits */
	switch (termios->c_cflag & CSIZE) {
	case CS7:
		ctrl2 &= ~BIT(0);  /* BIT8 = 0 for 7-bit */
		break;
	case CS8:
		ctrl2 |= BIT(0);   /* BIT8 = 1 for 8-bit */
		break;
	default:
		dev_warn(port->dev, "Unsupported data size, using 8 bits\n");
		ctrl2 |= BIT(0);
	}

	/* Parity (PARITY_EN and ODD_N_EVEN) */
	if (termios->c_cflag & PARENB) {
		ctrl2 |= BIT(1);   /* Enable parity */
		if (termios->c_cflag & PARODD)
			ctrl2 |= BIT(2);  /* Odd parity */
		else
			ctrl2 &= ~BIT(2); /* Even parity */
	} else {
		ctrl2 &= ~BIT(1); /* Disable parity */
	}

	ctrl2 |= ((baud_div >> 8) & MCHP_COREUART_BAUD_VALUE_UPPER) << 3;
	/* Writing Lower 8 bits of baud value */
	writeb(baud_div & MCHP_COREUART_BAUD_VALUE_LOWER, port->membase + MCHP_COREUART_CTRL1);
	/* Control config + upper 5 bits of baud value */
	writeb(ctrl2, port->membase + MCHP_COREUART_CTRL2);
	uart_update_timeout(port, termios->c_cflag, baud);
}

static int mchp_coreuart_startup(struct uart_port *port)
{
	int ret;

	ret = request_irq(port->irq, mchp_coreuart_interrupt, 0, "coreuart", port);
	if (ret) {
		dev_err(port->dev, "Failed to request IRQ\n");
		return ret;
	}

	return 0;
}

static void mchp_coreuart_shutdown(struct uart_port *port)
{
	free_irq(port->irq, port);
}

static const char *mchp_coreuart_type(struct uart_port *port)
{
	return (port->type == PORT_MCHP_COREUART) ? "mchp_coreuart" : NULL;
}

static void mchp_coreuart_config_port(struct uart_port *port, int flags)
{
	if (!(flags & UART_CONFIG_TYPE))
		return;

	if (mchp_coreuart_request_port(port))
		return;

	port->type = PORT_MCHP_COREUART;
}

static void wait_for_txready(struct uart_port *port)
{
	unsigned int tmout = MCHP_COREUART_TXRDY_WAIT_TIMEOUT;

	while (--tmout) {
		unsigned int val;

		val = readb(port->membase + MCHP_COREUART_STATUS);
		if (val & MCHP_COREUART_STATUS_TXRDY_MASK)
			break;
	}
}

static void mchp_coreuart_printonconsole(struct uart_port *port, unsigned char ch)
{
	wait_for_txready(port);
	writeb(ch, port->membase + MCHP_COREUART_TXDATA);
}

static void mchp_coreuart_console_write(struct console *co, const char *s, unsigned int count)
{
	struct uart_port *port;
	unsigned long flags;
	int locked = 1;

	port = &ports[co->index];

	if (oops_in_progress)
		locked = uart_port_trylock_irqsave(port, &flags);
	else
		uart_port_lock_irqsave(port, &flags);

	uart_console_write(port, s, count, mchp_coreuart_printonconsole);
	wait_for_txready(port);

	if (locked)
		uart_port_unlock_irqrestore(port, flags);
}

static int mchp_coreuart_console_setup(struct console *co, char *options)
{
	struct uart_port *port;
	int baud = 115200;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';

	if (co->index < 0 || co->index >= 6)
		return -EINVAL;

	port = &ports[co->index];
	if (!port->ops || !port->membase)
		return -ENODEV;

	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);

	return uart_set_options(port, co, baud, parity, bits, flow);
}

static struct console mchp_coreuart_console = {
	.name		= "ttyCOREUART",
	.write		= mchp_coreuart_console_write,
	.device		= uart_console_device,
	.setup		= mchp_coreuart_console_setup,
	.flags		= CON_PRINTBUFFER,
	.index		= -1,
	.data		= &mchp_core_uart_driver,
};

static void mchp_coreuart_early_write(struct console *con, const char *s, unsigned int n)
{
	struct earlycon_device *dev = con->data;

	uart_console_write(&dev->port, s, n, mchp_coreuart_printonconsole);
}

static int __init mchp_coreuart_early_console_setup(struct earlycon_device *device, const char *opt)
{
	if (!device->port.membase)
		return -ENODEV;

	device->con->write = mchp_coreuart_early_write;
	return 0;
}

OF_EARLYCON_DECLARE(mchp_coreuart, "microchip,coreuart-rtl-v5", mchp_coreuart_early_console_setup);

static const struct uart_ops mchp_coreuart_ops = {
	.tx_empty	=	mchp_coreuart_tx_empty,
	.get_mctrl	=	mchp_coreuart_get_mctrl,
	.set_mctrl	=	mchp_coreuart_set_mctrl,
	.start_tx	=	mchp_coreuart_start_tx,
	.stop_tx	=	mchp_coreuart_stop_tx,
	.stop_rx	=	mchp_coreuart_stop_rx,
	.set_termios	=	mchp_coreuart_set_termios,
	.startup	=	mchp_coreuart_startup,
	.shutdown	=	mchp_coreuart_shutdown,
	.type		=	mchp_coreuart_type,
	.release_port	=	mchp_coreuart_release_port,
	.request_port	=	mchp_coreuart_request_port,
	.config_port	=	mchp_coreuart_config_port,
};

#define MCHP_COREUART_CONSOLE (&mchp_coreuart_console)

static struct uart_driver mchp_core_uart_driver = {
	.owner		=	THIS_MODULE,
	.driver_name	=	"mchp_coreuart",
	.dev_name	=	"ttyCOREUART",
	.major		=	0,
	.minor		=	0,
	.nr		=	MCHP_COREUART_MAX_SERIAL_INSTANCE,
	.cons		=	MCHP_COREUART_CONSOLE,
};

static int mchp_coreuart_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	struct mchp_coreuart_port *apb_uart;
	int ret;

	if (pdev->dev.of_node)
		pdev->id = of_alias_get_id(pdev->dev.of_node, "serial");

	if (pdev->id < 0)
		dev_err_probe(dev, -EINVAL, "Invalid device id: %d\n", pdev->id);

	if (pdev->id >= MCHP_COREUART_MAX_SERIAL_INSTANCE)
		dev_err_probe(dev, -EINVAL,
			      "Device id %d exceeds maximum (%d), please increase MCHP_COREUART_MAX_SERIAL_INSTANCE\n",
			      pdev->id, MCHP_COREUART_MAX_SERIAL_INSTANCE);

	apb_uart = devm_kzalloc(dev, sizeof(*apb_uart), GFP_KERNEL);
	apb_uart->port.membase = devm_platform_get_and_ioremap_resource(pdev, 0, &res);

	if (IS_ERR(apb_uart->port.membase))
		return dev_err_probe(dev, PTR_ERR(apb_uart->port.membase), "Memory map failed\n");

	apb_uart->port.mapbase = res->start;
	apb_uart->port.iotype = UPIO_MEM;

	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get IRQ\n");

	apb_uart->port.irq = ret;
	apb_uart->clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(apb_uart->clk))
		return dev_err_probe(dev, PTR_ERR(apb_uart->clk), "Failed to get clock\n");

	apb_uart->port.uartclk = clk_get_rate(apb_uart->clk);
	if (apb_uart->port.uartclk == 0)
		return dev_err_probe(dev, -EINVAL, "Invalid UART clock rate\n");

	apb_uart->port.dev = &pdev->dev;
	apb_uart->port.fifosize = MCHP_COREUART_MPFS_FIFO_SIZE;
	apb_uart->port.flags = UPF_BOOT_AUTOCONF;
	apb_uart->port.ops =  &mchp_coreuart_ops;
	apb_uart->port.line = pdev->id >= 0 ? pdev->id : -1;
	apb_uart->port.has_sysrq = IS_ENABLED(CONFIG_SERIAL_COREUART_CONSOLE);
	ports[pdev->id] = apb_uart->port;

	/* Register the UART port */
	ret = uart_add_one_port(&mchp_core_uart_driver, &apb_uart->port);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to add UART port\n");

	platform_set_drvdata(pdev, &apb_uart->port);
	return 0;
}

static void mchp_coreuart_remove(struct platform_device *pdev)
{
	struct mchp_coreuart_port *apb_uart = platform_get_drvdata(pdev);

	uart_remove_one_port(&mchp_core_uart_driver, &apb_uart->port);
}

static const struct of_device_id mchp_coreuart_of_match[] = {
	{ .compatible = "microchip,coreuart-rtl-v5" },
	{},
};

MODULE_DEVICE_TABLE(of, mchp_coreuart_of_match);

static struct platform_driver mchp_coreuart_driver = {
	.driver = {
		.name = "mchp_coreuart",
		.of_match_table = mchp_coreuart_of_match,
	},
	.probe = mchp_coreuart_probe,
	.remove = mchp_coreuart_remove,
};

static int __init mchp_coreuart_init(void)
{
	int ret;

	ret = uart_register_driver(&mchp_core_uart_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&mchp_coreuart_driver);
	if (ret)
		uart_unregister_driver(&mchp_core_uart_driver);

	return ret;
}

static void __exit mchp_coreuart_exit(void)
{
	platform_driver_unregister(&mchp_coreuart_driver);
	uart_unregister_driver(&mchp_core_uart_driver);
}

module_init(mchp_coreuart_init);
module_exit(mchp_coreuart_exit);

MODULE_AUTHOR("prashanthkumar.burujukindi@microchip.com");
MODULE_DESCRIPTION("Microchip COREUART Driver");
MODULE_LICENSE("GPL");
