#include <asm/hwio.h>
#include <asm/irq.h>

#include <phabos/i2c.h>
#include <phabos/i2c/stm32-i2c.h>
#include <phabos/utils.h>

#define I2C_CR1     0x00
#define I2C_CR2     0x04
#define I2C_OAR1    0x08
#define I2C_OAR2    0x0c
#define I2C_DR      0x10
#define I2C_SR1     0x14
#define I2C_SR2     0x18
#define I2C_CCR     0x1c
#define I2C_TRISE   0x20
#define I2C_FLTR    0x24

#define I2C_CR1_PE          (1 << 0)
#define I2C_CR1_START       (1 << 8)
#define I2C_CR1_STOP        (1 << 9)

#define I2C_CR2_ITERREN     (1 << 8)
#define I2C_CR2_ITEVTEN     (1 << 9)

#define I2C_SR1_SB          (1 << 0)
#define I2C_SR1_ADDR        (1 << 1)
#define I2C_SR1_BTF         (1 << 2)
#define I2C_SR1_RXE         (1 << 6)
#define I2C_SR1_TXE         (1 << 7)
#define I2C_SR1_BERR        (1 << 8)
#define I2C_SR1_ARLO        (1 << 9)
#define I2C_SR1_AF          (1 << 10)
#define I2C_SR1_OVR         (1 << 11)
#define I2C_SR1_PECERR      (1 << 12)
#define I2C_SR1_TIMEOUT     (1 << 14)

#define I2C_CCR_DUTY        (1 << 14)
#define I2C_CCR_STDMODE     (0 << 15)
#define I2C_CCR_FASTMODE    (1 << 15)

#define ONE_MHZ 1000000

#define I2C_FASTMODE_INVT   3333333
#define I2C_STDMODE_INVT    1000000

#define SR1_ERROR_MASK      (I2C_SR1_TIMEOUT | I2C_SR1_AF | I2C_SR1_PECERR | \
                            I2C_SR1_OVR | I2C_SR1_ARLO | I2C_SR1_BERR)

__driver__ struct driver stm32_i2c_driver;

struct stm32_adapter_priv {
    struct mutex lock;
    struct semaphore xfer_semaphore;
};

static void i2c_dump(struct device *device)
{
    kprintf("\tCR1: %#X\n", read32(device->reg_base + I2C_CR1));
    kprintf("\tCR2: %#X\n", read32(device->reg_base + I2C_CR2));
    kprintf("\tSR1: %#X\n", read32(device->reg_base + I2C_SR1));
    kprintf("\tSR2: %#X\n", read32(device->reg_base + I2C_SR2));
    kprintf("\tCCR: %#X\n", read32(device->reg_base + I2C_CCR));
    kprintf("\tTRISE: %#X\n", read32(device->reg_base + I2C_TRISE));
}

static void stm32_i2c_err_irq(int irq, void *data)
{
    struct device *device = data;
    struct stm32_adapter_priv *priv = device->priv;

    kprintf("ERR\n");

    semaphore_up(&priv->xfer_semaphore);
}

static void stm32_i2c_evt_irq(int irq, void *data)
{
    struct device *device = data;
    struct stm32_adapter_priv *priv = device->priv;

    kprintf("EVT\n");
    i2c_dump(device);

    irq_disable_line(irq);

    semaphore_up(&priv->xfer_semaphore);
}

static int stm32_i2c_set_freq(struct device *device, unsigned int freq)
{
    struct stm32_i2c_adapter_platform *pdata = device->pdata;
    uint32_t ccr;
    uint32_t trise;
    unsigned speed;
    uint32_t cr1;
    int retval = 0;

    kprintf("%s()\n", __func__);

    // Disable I2C controller
    cr1 = read32(device->reg_base + I2C_CR1);
    write32(device->reg_base + I2C_CR1, 0);

    if (freq > I2C_FASTMODE_MAX_FREQ) {
        retval = -EINVAL;
        goto out;
    }

    // Configure I2C clock given APB1 freq, and I2C bus freq requested
    speed = I2C_CCR_STDMODE;
    if (freq > I2C_STDMODE_MAX_FREQ)
        speed = I2C_CCR_FASTMODE;

    if (freq == I2C_FASTMODE_MAX_FREQ && 0)
        ccr = pdata->clk / (freq * 25) | I2C_CCR_DUTY;
    else
        ccr = pdata->clk / (freq * 3);

    write32(device->reg_base + I2C_CCR, speed | ccr);

    // Configure Max Rise Time
    if (freq > I2C_FASTMODE_MAX_FREQ)
        trise = (pdata->clk / I2C_FASTMODE_INVT) + 1;
    else
        trise = (pdata->clk / I2C_STDMODE_INVT) + 1;

    write32(device->reg_base + I2C_TRISE, trise);

out:
    write32(device->reg_base + I2C_CR1, cr1);
    return retval;
}

static struct stm32_adapter_priv *stm32_adapter_priv_alloc(void)
{
    struct stm32_adapter_priv *priv;

    priv = kzalloc(sizeof(*priv), 0);
    if (!priv)
        return NULL;

    mutex_init(&priv->lock);
    semaphore_init(&priv->xfer_semaphore, 0);
    return priv;
}

static int stm32_i2c_open(struct file *file)
{
    struct device *device;
    struct stm32_i2c_adapter_platform *pdata;

    kprintf("%s()\n", __func__);

    RET_IF_FAIL(file, -EINVAL);
    RET_IF_FAIL(file->inode, -EINVAL);

    device = devnum_get_device(file->inode->dev);
    RET_IF_FAIL(device, -EINVAL);

    pdata = device->pdata;
    if (!pdata)
        return -EINVAL;

    device->priv = stm32_adapter_priv_alloc();
    if (!device->priv)
        return -ENOMEM;

    write32(device->reg_base + I2C_CR1, 0);

    i2c_dump(device);

    // Set APB1 clock + enable EVT and ERR interrupts
    write32(device->reg_base + I2C_CR2, (pdata->clk / ONE_MHZ) |
                                        I2C_CR2_ITEVTEN | I2C_CR2_ITERREN);

    stm32_i2c_set_freq(device, I2C_STDMODE_MAX_FREQ);

    // Enable controller
    write32(device->reg_base + I2C_CR1, I2C_CR1_PE);

i2c_dump(device);

    return 0;
}

static int stm32_i2c_close(struct file *file)
{
    struct device *device;

    kprintf("%s()\n", __func__);

    RET_IF_FAIL(file, -EINVAL);
    RET_IF_FAIL(file->inode, -EINVAL);

    device = devnum_get_device(file->inode->dev);
    RET_IF_FAIL(device, -EINVAL);

    write32(device->reg_base + I2C_CR1, 0);

    kfree(device->priv);

    return 0;
}

static inline int stm32_i2c_generate_start_condition(struct device *device)
{
//    struct stm32_adapter_priv *priv = device->priv;
    uint32_t sr1;

    kprintf("%s()\n", __func__);

    write32(device->reg_base + I2C_CR1, I2C_CR1_PE | I2C_CR1_START);

//    semaphore_down(&priv->xfer_semaphore);

    sr1 = read32(device->reg_base + I2C_SR1);
    if (sr1 & SR1_ERROR_MASK) {
        return -EIO;
    }

    return 0;
}

static inline void stm32_i2c_generate_stop_condition(struct device *device)
{
    kprintf("%s()\n", __func__);

    write32(device->reg_base + I2C_CR1, I2C_CR1_PE | I2C_CR1_STOP);
}

static inline int stm32_i2c_send_rx_address(struct device *device,
                                            uint16_t addr)
{
//    struct stm32_adapter_priv *priv = device->priv;
    uint32_t sr1;

    kprintf("%s()\n", __func__);

    write32(device->reg_base + I2C_DR, (addr << 1) | 1);
//    semaphore_down(&priv->xfer_semaphore);

    sr1 = read32(device->reg_base + I2C_SR1);
    if (sr1 & SR1_ERROR_MASK) {
        return -EIO;
    }

    read32(device->reg_base + I2C_SR2);
    return 0;
}

static inline int stm32_i2c_send_tx_address(struct device *device,
                                            uint16_t addr)
{
//    struct stm32_adapter_priv *priv = device->priv;
    uint32_t sr1;

    kprintf("%s()\n", __func__);

    write32(device->reg_base + I2C_DR, addr << 1);
//    semaphore_down(&priv->xfer_semaphore);

    sr1 = read32(device->reg_base + I2C_SR1);
    if (sr1 & SR1_ERROR_MASK) {
        kprintf("%u %d %u\n", sr1, SR1_ERROR_MASK, sr1 & SR1_ERROR_MASK);
        return -EIO;
    }

    read32(device->reg_base + I2C_SR2);
    return 0;
}

static int stm32_i2c_recv(struct device *device, struct i2c_msg *msg)
{
    int retval;

    kprintf("%s()\n", __func__);

    retval = stm32_i2c_send_rx_address(device, msg->addr);
    if (retval)
        return retval;

    for (unsigned i = 0; i < msg->length; i++)
        msg->buffer[i] = read32(device->reg_base + I2C_DR);

    return 0;
}

static int stm32_i2c_send(struct device *device, struct i2c_msg *msg)
{
    int retval;

    kprintf("%s()\n", __func__);

    irq_disable();

    retval = stm32_i2c_send_tx_address(device, msg->addr);
    if (retval)
        return retval;

    i2c_dump(device);

    for (unsigned i = 0; i < msg->length; i++) {
        while (!(read32(device->reg_base + I2C_SR1) & I2C_SR1_TXE)) {
        }
        write32(device->reg_base + I2C_DR, msg->buffer[i]);
    }

    irq_enable();

    return 0;
}

static ssize_t stm32_i2c_transfer(struct device *device, struct i2c_msg *msg,
                                  size_t count)
{
    ssize_t retval = 0;
    struct stm32_adapter_priv *priv = device->priv;

    kprintf("%s()\n", __func__);

    if (!count)
        return 0;

i2c_dump(device);

    mutex_lock(&priv->lock);

    for (unsigned i = 0; i < count; i++) {
        retval = stm32_i2c_generate_start_condition(device); // repeated start
        if (retval)
            break;

        if (msg[i].flags & I2C_M_READ)
            retval = stm32_i2c_recv(device, &msg[i]);
        else
            retval = stm32_i2c_send(device, &msg[i]);

        if (retval)
            break;
    }

i2c_dump(device);

    stm32_i2c_generate_stop_condition(device);

    mutex_unlock(&priv->lock);

    return retval;
}

int stm32_i2c_ioctl(struct file *file, unsigned long cmd, va_list vl)
{
    struct device *device = devnum_get_device(file->inode->dev);
    int retval;
    size_t msg_count;
    struct i2c_msg *msg;

    kprintf("%s()\n", __func__);

    RET_IF_FAIL(file, -EINVAL);

    switch (cmd) {
    case I2C_TRANSFER:
        msg = va_arg(vl, struct i2c_msg*);
        msg_count = va_arg(vl, size_t);
        retval = stm32_i2c_transfer(device, msg, msg_count);
        break;

    case I2C_SET_FREQUENCY:
        retval = stm32_i2c_set_freq(device, va_arg(vl, unsigned));
        break;

    default:
        retval = -EINVAL;
    }

    return 0;
}

static struct file_operations stm32_i2c_ops = {
    .open = stm32_i2c_open,
    .close = stm32_i2c_close,
    .ioctl = stm32_i2c_ioctl,
};

static int stm32_i2c_probe(struct device *device)
{
    int retval;
    struct stm32_i2c_adapter_platform *pdata;
    struct i2c_adapter *adapter =
        containerof(device, struct i2c_adapter, device);
    dev_t devnum;

    RET_IF_FAIL(adapter, -EINVAL);

    device->ops = stm32_i2c_ops;

    retval = devnum_alloc(&stm32_i2c_driver, device, &devnum);
    if (retval)
        return -ENOMEM;

    pdata = device->pdata;
    if (!pdata)
        return -EINVAL;

    irq_attach(pdata->evt_irq, stm32_i2c_evt_irq, device);
    irq_attach(pdata->err_irq, stm32_i2c_err_irq, device);

    irq_enable_line(pdata->evt_irq);
    irq_enable_line(pdata->err_irq);

    return i2c_adapter_register(adapter, devnum);
}

static int stm32_i2c_remove(struct device *device)
{
    struct stm32_i2c_adapter_platform *pdata;

    RET_IF_FAIL(device, -EINVAL);

    pdata = device->pdata;
    if (!pdata)
        return -EINVAL;

    irq_detach(pdata->evt_irq);
    irq_detach(pdata->err_irq);

    irq_disable_line(pdata->evt_irq);
    irq_disable_line(pdata->err_irq);

    return 0;
}

__driver__ struct driver stm32_i2c_driver = {
    .name = "stm32-i2c",

    .probe = stm32_i2c_probe,
    .remove = stm32_i2c_remove,
};