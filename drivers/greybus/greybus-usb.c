#include <errno.h>
#include <phabos/greybus.h>
#include <phabos/utils.h>
#include <phabos/assert.h>
#include <phabos/usb/hcd.h>
#include <phabos/usb/std-requests.h>
#include <phabos/usb/driver.h>

static struct usb_device *usbdev;

int gb_ap_init(void);

void gb_usb_send_complete(struct urb *urb)
{
    kprintf("%s() = %d\n", __func__, urb->status);
}

void gb_usb_rx_complete(struct urb *urb)
{
    kprintf("%s() = %d\n", __func__, urb->status);
    urb->device->hcd->driver->urb_enqueue(usbdev->hcd, urb);
}

int gb_usb_send(unsigned int cportid, const void *buf, size_t len)
{
    static struct urb *urb = NULL;

    kprintf("%s()\n", __func__);

    if (!urb) {
        urb = urb_create(usbdev);
    } else {
//        memset(urb, 0, sizeof(*urb));
//        urb->hcpriv_ep = NULL;
    }

    atomic_init(&urb->refcount, 1);
    semaphore_init(&urb->semaphore, 0);
    urb->device = usbdev;
    urb->complete = gb_usb_send_complete;
    urb->pipe = (USB_HOST_PIPE_BULK << 30) | (2 << 15) |
                (usbdev->address << 8) | USB_HOST_DIR_OUT;
    urb->maxpacket = 0x40;
    urb->flags = USB_URB_GIVEBACK_ASAP;
    urb->buffer = (void*) buf;
    urb->length = len;

    struct gb_operation_hdr *hdr = (struct gb_operation_hdr*) buf;
    hdr->pad[1] = cportid >> 8;
    hdr->pad[0] = cportid & 0xff;

    usbdev->hcd->driver->urb_enqueue(usbdev->hcd, urb);

    return 0;
}

int gb_in(const void *buf, size_t len)
{
    struct urb *urb = urb_create(usbdev);

    kprintf("%s()\n", __func__);

    urb->complete = gb_usb_rx_complete;
    urb->pipe = (USB_HOST_PIPE_BULK << 30) | (3 << 15) |
                (usbdev->address << 8) | USB_HOST_DIR_IN;
    urb->buffer = (void*) buf;
    urb->length = len;
    urb->maxpacket = 0x40;
    urb->flags = USB_URB_GIVEBACK_ASAP;

    usbdev->hcd->driver->urb_enqueue(usbdev->hcd, urb);

    return 0;
}

void gb_usb_dev(void)
{
    kprintf("%s()\n", __func__);

#if 0
    void *buffer = malloc(255);
    struct usb_device_descriptor *desc = buffer;

    usb_control_msg(dev, USB_DEVICE_GET_DESCRIPTOR,
                    USB_DESCRIPTOR_DEVICE << 8, 0, sizeof(*desc), desc);

    if (desc->idVendor != 0xffff)
        return -EINVAL;

    usb_control_msg(dev, USB_DEVICE_GET_DESCRIPTOR,
                    USB_DESCRIPTOR_CONFIGURATION << 8, 0, 255, buffer);

    print_descriptor(buffer);
#endif

//    uint32_t buffer[255];
//    kprintf("buffer: %x\n", buffer);
//    gb_in(buffer, 255);
}

static struct gb_transport_backend gb_usb_backend = {
    .init = gb_usb_dev,
    .send = gb_usb_send,
};

void print_descriptor(void *raw_descriptor);

static int gb_usb_init_bus(struct usb_device *dev)
{
    void *buffer = malloc(255);
    struct usb_device_descriptor *desc = buffer;

    usbdev = dev;

    usb_control_msg(dev, USB_DEVICE_SET_CONFIGURATION, 1, 0, 0, NULL);

    usb_control_msg(dev, USB_DEVICE_GET_DESCRIPTOR,
                    USB_DESCRIPTOR_DEVICE << 8, 0, sizeof(*desc), desc);

    if (desc->idVendor != 0xffff)
        return -EINVAL;

    usb_control_msg(dev, USB_DEVICE_GET_DESCRIPTOR,
                    USB_DESCRIPTOR_CONFIGURATION << 8, 0, 255, buffer);

    print_descriptor(buffer);

    return gb_init(&gb_usb_backend);
}

static struct usb_class_driver greybus_usb_class_driver = {
    .class = 0,
    .init = gb_usb_init_bus,
};

static int gb_usb_init(struct driver *driver)
{
    int retval;

    retval = usb_register_class_driver(&greybus_usb_class_driver);
    if (retval)
        return retval;

    retval = gb_init(&gb_usb_backend);
    if (retval)
        return retval;

    gb_ap_init();

    return 0;
}

__driver__ struct driver gb_usb_driver = {
    .name = "gb-usb",
    .init = gb_usb_init,
};
